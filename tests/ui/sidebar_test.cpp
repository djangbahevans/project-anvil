#include "anvil/ui/activity_bar.hpp"
#include "anvil/ui/sidebar.hpp"

#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <gtest/gtest.h>
#include <memory>

using namespace anvil::ui;

namespace {

QIcon square_icon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::black);
    return QIcon(pixmap);
}

class SidebarTest : public ::testing::Test {
protected:
    void SetUp() override {
        workbench_ = std::make_unique<Workbench>(colors_);
        workbench_->set_editor_area(new QLabel("editor"));
        workbench_->resize(1000, 700);
        sidebar_ = std::make_unique<Sidebar>(*workbench_, registry_, colors_);
        workbench_->show();
        QApplication::processEvents();
    }

    // Registers a container whose widget is a label with the id, and counts how often it is made.
    void add(const std::string& id, int order = 0,
             ViewContainerLocation location = ViewContainerLocation::sidebar) {
        ViewContainer container;
        container.id = id;
        container.title = QString::fromStdString("Title of " + id);
        container.icon = square_icon();
        container.order = order;
        container.location = location;
        container.create = [this, id] {
            ++created_[id];
            return new QLabel(QString::fromStdString(id));
        };
        registry_.add(std::move(container));
        QApplication::processEvents();
    }

    QToolButton* button(const std::string& id) const {
        return sidebar_->activity_bar().button(id);
    }
    void click(const std::string& id) {
        ASSERT_NE(button(id), nullptr) << id;
        button(id)->click();
        QApplication::processEvents();
    }
    WorkbenchColors colors_{QApplication::palette()};
    ViewContainerRegistry registry_;
    std::unique_ptr<Workbench> workbench_;
    std::unique_ptr<Sidebar> sidebar_;
    std::map<std::string, int> created_;
};

} // namespace

TEST_F(SidebarTest, TheActivityBarShowsWithTheFirstSidebarContainerAndLeavesWithTheLast) {
    EXPECT_FALSE(workbench_->is_visible(Part::activity_bar));
    add("explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::activity_bar));
    add("search");
    registry_.remove("explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::activity_bar));
    registry_.remove("search");
    EXPECT_FALSE(workbench_->is_visible(Part::activity_bar));
}

TEST_F(SidebarTest, APanelContainerDoesNotShowTheActivityBar) {
    add("terminal", 0, ViewContainerLocation::panel);
    EXPECT_FALSE(workbench_->is_visible(Part::activity_bar));
    EXPECT_EQ(sidebar_->activity_bar().button_count(), 0U);
}

TEST_F(SidebarTest, TheButtonsFollowTheOrderAndShowTheTitleAsTheirToolTip) {
    add("c", 2);
    add("a", 0);
    add("b", 1);
    EXPECT_EQ(sidebar_->activity_bar().ids(), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(button("b")->toolTip(), "Title of b");
    EXPECT_EQ(button("b")->width(), ActivityBar::button_size);
    EXPECT_EQ(button("b")->height(), ActivityBar::button_size);
    EXPECT_LT(button("a")->y(), button("b")->y());
    EXPECT_LT(button("b")->y(), button("c")->y());
}

TEST_F(SidebarTest, ClickingAButtonShowsTheSidebarWithTheContainer) {
    add("explorer");
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
    click("explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    EXPECT_EQ(sidebar_->active_container(), "explorer");
    ASSERT_NE(sidebar_->container_widget("explorer"), nullptr);
    EXPECT_TRUE(sidebar_->container_widget("explorer")->isVisible());
    EXPECT_TRUE(workbench_->sidebar()->isAncestorOf(sidebar_->container_widget("explorer")));
    EXPECT_EQ(sidebar_->title_actions()->parentWidget()->height(), 35);
}

TEST_F(SidebarTest, ThereIsNoWidgetUntilTheContainerIsShown) {
    add("explorer");
    add("search");
    EXPECT_EQ(created_["explorer"], 0);
    EXPECT_EQ(sidebar_->container_widget("explorer"), nullptr);

    click("explorer");
    EXPECT_EQ(created_["explorer"], 1);
    EXPECT_EQ(created_["search"], 0);
    EXPECT_EQ(sidebar_->container_widget("search"), nullptr);

    click("search");
    click("explorer");
    click("search");
    EXPECT_EQ(created_["explorer"], 1);
    EXPECT_EQ(created_["search"], 1);
}

TEST_F(SidebarTest, ClickingTheActiveButtonHidesTheSidebarAndAgainShowsTheSameWidget) {
    add("explorer");
    click("explorer");
    QWidget* widget = sidebar_->container_widget("explorer");
    click("explorer");
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
    EXPECT_EQ(sidebar_->active_container(), "explorer");
    click("explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    EXPECT_EQ(sidebar_->container_widget("explorer"), widget);
    EXPECT_EQ(created_["explorer"], 1);
}

TEST_F(SidebarTest, SwitchingContainersChangesTheWidgetShown) {
    add("explorer");
    add("search");
    click("explorer");
    click("search");
    EXPECT_TRUE(sidebar_->container_widget("search")->isVisible());
    EXPECT_FALSE(sidebar_->container_widget("explorer")->isVisible());
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    click("explorer");
    EXPECT_TRUE(sidebar_->container_widget("explorer")->isVisible());
    EXPECT_FALSE(sidebar_->container_widget("search")->isVisible());
}

TEST_F(SidebarTest, OnlyTheActiveButtonIsCheckedWhileTheSidebarIsShown) {
    add("explorer");
    add("search");
    EXPECT_FALSE(button("explorer")->isChecked());
    click("search");
    EXPECT_TRUE(button("search")->isChecked());
    EXPECT_FALSE(button("explorer")->isChecked());
    click("search");
    EXPECT_FALSE(button("search")->isChecked());
    workbench_->set_visible(Part::sidebar, true);
    EXPECT_TRUE(button("search")->isChecked());
}

TEST_F(SidebarTest, TheActiveContainerIsAPropertyWithASignal) {
    add("explorer");
    add("search");
    QSignalSpy changed(sidebar_.get(), &Sidebar::active_container_changed);
    EXPECT_EQ(sidebar_->active_container(), "");

    click("explorer");
    ASSERT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).toString(), "explorer");
    click("explorer");
    EXPECT_EQ(changed.count(), 1);
    click("explorer");
    EXPECT_EQ(changed.count(), 1);
    click("search");
    ASSERT_EQ(changed.count(), 2);
    EXPECT_EQ(changed.at(1).at(0).toString(), "search");

    sidebar_->activate("search");
    sidebar_->activate("nothing");
    EXPECT_EQ(changed.count(), 2);
}

TEST_F(SidebarTest, ShowingTheSidebarAnotherWayShowsTheLastContainerOrTheFirst) {
    add("explorer", 0);
    add("search", 1);
    workbench_->set_visible(Part::sidebar, true);
    EXPECT_EQ(sidebar_->active_container(), "explorer");
    ASSERT_NE(sidebar_->container_widget("explorer"), nullptr);
    EXPECT_TRUE(button("explorer")->isChecked());

    click("search");
    workbench_->set_visible(Part::sidebar, false);
    workbench_->set_visible(Part::sidebar, true);
    EXPECT_EQ(sidebar_->active_container(), "search");
    EXPECT_TRUE(sidebar_->container_widget("search")->isVisible());
}

TEST_F(SidebarTest, ShowingTheSidebarWithNoContainersLeavesItEmpty) {
    workbench_->set_visible(Part::sidebar, true);
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    EXPECT_EQ(sidebar_->active_container(), "");
}

TEST_F(SidebarTest, RemovingTheActiveContainerShowsAnotherOrHidesTheSidebar) {
    add("explorer", 0);
    add("search", 1);
    click("search");
    QPointer<QWidget> widget = sidebar_->container_widget("search");
    QSignalSpy changed(sidebar_.get(), &Sidebar::active_container_changed);

    registry_.remove("search");
    QApplication::processEvents();
    EXPECT_EQ(widget.data(), nullptr);
    EXPECT_EQ(sidebar_->active_container(), "explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
    EXPECT_TRUE(sidebar_->container_widget("explorer")->isVisible());
    EXPECT_EQ(changed.count(), 2);

    registry_.remove("explorer");
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
    EXPECT_EQ(sidebar_->active_container(), "");
}

TEST_F(SidebarTest, RemovingAContainerThatIsNotShownKeepsTheActiveOne) {
    add("explorer", 0);
    add("search", 1);
    click("explorer");
    registry_.remove("search");
    EXPECT_EQ(sidebar_->active_container(), "explorer");
    EXPECT_TRUE(workbench_->is_visible(Part::sidebar));
}

TEST_F(SidebarTest, TheButtonsAreAccessible) {
    QAccessible::setActive(true);
    add("explorer");
    add("search");
    click("search");

    const auto* explorer = QAccessible::queryAccessibleInterface(button("explorer"));
    const auto* search = QAccessible::queryAccessibleInterface(button("search"));
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(explorer->text(QAccessible::Name), "Title of explorer");
    EXPECT_EQ(explorer->role(), QAccessible::CheckBox);
    EXPECT_TRUE(explorer->state().checkable);
    EXPECT_FALSE(explorer->state().checked);
    EXPECT_TRUE(search->state().checked);
}

TEST_F(SidebarTest, TheActivityBarDrawsTheIndicatorAndTheIconInThemeColors) {
    add("explorer");
    add("search");
    click("explorer");
    colors_.set_colors({{"activityBar.background", QColor("#101010")},
                        {"activityBar.foreground", QColor("#ffffff")},
                        {"activityBar.inactiveForeground", QColor("#888888")},
                        {"activityBar.activeBorder", QColor("#ff8800")}});
    QApplication::processEvents();

    const QImage image = workbench_->activity_bar()->grab().toImage();
    const QPoint explorer = button("explorer")->pos();
    const QPoint search = button("search")->pos();
    const int middle = ActivityBar::button_size / 2;
    EXPECT_EQ(image.pixelColor(explorer + QPoint(1, middle)), QColor("#ff8800"));
    EXPECT_EQ(image.pixelColor(search + QPoint(1, middle)), QColor("#101010"));
    EXPECT_EQ(image.pixelColor(explorer + QPoint(middle, middle)), QColor("#ffffff"));
    EXPECT_EQ(image.pixelColor(search + QPoint(middle, middle)), QColor("#888888"));
    EXPECT_EQ(image.pixelColor(explorer + QPoint(middle, 2)), QColor("#101010"));
}

TEST_F(SidebarTest, TheTitleIsTheContainersTitleInCapitals) {
    add("explorer");
    add("search");
    click("explorer");
    EXPECT_EQ(sidebar_->title(), "TITLE OF EXPLORER");
    click("search");
    EXPECT_EQ(sidebar_->title(), "TITLE OF SEARCH");
}

TEST_F(SidebarTest, TheTitleIsDrawnInTheTitleColor) {
    add("explorer");
    click("explorer");
    colors_.set_colors({{"sideBar.background", QColor("#202020")},
                        {"sideBarTitle.foreground", QColor("#33ff33")}});
    QApplication::processEvents();

    QWidget* sidebar = workbench_->sidebar();
    const QImage image = sidebar->grab(QRect(0, 0, sidebar->width(), 35)).toImage();
    bool has_title_pixels = false;
    for (int y = 0; y < image.height() && !has_title_pixels; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            has_title_pixels = has_title_pixels || image.pixelColor(x, y) == QColor("#33ff33");
        }
    }
    EXPECT_TRUE(has_title_pixels);
    EXPECT_EQ(image.pixelColor(image.width() / 2, 2), QColor("#202020"));
}

TEST_F(SidebarTest, RendersTheActivityBarAndTheSidebar) {
    add("explorer", 0);
    add("search", 1);
    add("extensions", 2);
    click("search");
    const QImage image = workbench_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/sidebar.png");
    }
}

TEST_F(SidebarTest, AContainerOfAnotherLocationIsNotShown) {
    add("terminal", 0, ViewContainerLocation::panel);
    sidebar_->activate("terminal");
    QApplication::processEvents();
    EXPECT_EQ(sidebar_->active_container(), "");
    EXPECT_EQ(sidebar_->container_widget("terminal"), nullptr);
    EXPECT_EQ(created_["terminal"], 0);
    EXPECT_FALSE(workbench_->is_visible(Part::sidebar));
}
