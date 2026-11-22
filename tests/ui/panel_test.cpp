#include "anvil/ui/panel.hpp"

#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <map>
#include <memory>

using namespace anvil::ui;

namespace {

class PanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        workbench_ = std::make_unique<Workbench>(colors_);
        editor_ = new QLabel("editor");
        workbench_->set_editor_area(editor_);
        workbench_->resize(1000, 700);
        panel_ = std::make_unique<Panel>(*workbench_, registry_, colors_);
        workbench_->show();
        QApplication::processEvents();
    }

    void add(const std::string& id, int order = 0,
             ViewContainerLocation location = ViewContainerLocation::panel) {
        ViewContainer container;
        container.id = id;
        container.title = QString::fromStdString("Title of " + id);
        container.order = order;
        container.location = location;
        container.create = [this, id] {
            ++created_[id];
            return new QLabel(QString::fromStdString(id));
        };
        registry_.add(std::move(container));
        QApplication::processEvents();
    }
    void click(const std::string& id) {
        ASSERT_NE(panel_->tab(id), nullptr) << id;
        panel_->tab(id)->click();
        QApplication::processEvents();
    }

    WorkbenchColors colors_{QApplication::palette()};
    ViewContainerRegistry registry_;
    std::unique_ptr<Workbench> workbench_;
    std::unique_ptr<Panel> panel_;
    QLabel* editor_ = nullptr;
    std::map<std::string, int> created_;
};

} // namespace

TEST_F(PanelTest, TheTabsAreThePanelContainersInOrderAndSidebarContainersAreIgnored) {
    add("c", 2);
    add("a", 0);
    add("b", 1);
    add("explorer", 0, ViewContainerLocation::sidebar);
    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_EQ(panel_->tab_ids(), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(panel_->tab_title("b"), "TITLE OF B");
    EXPECT_LT(panel_->tab("a")->x(), panel_->tab("b")->x());
    EXPECT_LT(panel_->tab("b")->x(), panel_->tab("c")->x());
}

TEST_F(PanelTest, ChoosingATabShowsThePanelWithTheContainer) {
    add("terminal");
    add("output", 1);
    click("terminal");
    EXPECT_TRUE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(panel_->active_container(), "terminal");
    ASSERT_NE(panel_->container_widget("terminal"), nullptr);
    EXPECT_TRUE(panel_->container_widget("terminal")->isVisible());
    EXPECT_TRUE(workbench_->panel()->isAncestorOf(panel_->container_widget("terminal")));
    EXPECT_TRUE(panel_->tab("terminal")->isChecked());
    EXPECT_FALSE(panel_->tab("output")->isChecked());
}

TEST_F(PanelTest, ThereIsNoWidgetUntilTheContainerIsShown) {
    add("terminal");
    add("output", 1);
    EXPECT_EQ(created_["terminal"], 0);
    EXPECT_EQ(panel_->container_widget("terminal"), nullptr);

    click("terminal");
    EXPECT_EQ(created_["terminal"], 1);
    EXPECT_EQ(created_["output"], 0);

    click("output");
    click("terminal");
    click("output");
    EXPECT_EQ(created_["terminal"], 1);
    EXPECT_EQ(created_["output"], 1);
}

TEST_F(PanelTest, SwitchingTabsChangesTheContainerShown) {
    add("terminal");
    add("output", 1);
    click("terminal");
    click("output");
    ASSERT_NE(panel_->container_widget("terminal"), nullptr);
    ASSERT_NE(panel_->container_widget("output"), nullptr);
    EXPECT_TRUE(panel_->container_widget("output")->isVisible());
    EXPECT_FALSE(panel_->container_widget("terminal")->isVisible());
    EXPECT_TRUE(panel_->tab("output")->isChecked());
    EXPECT_FALSE(panel_->tab("terminal")->isChecked());
}

TEST_F(PanelTest, ClickingTheActiveTabKeepsThePanelShown) {
    add("terminal");
    click("terminal");
    click("terminal");
    EXPECT_TRUE(workbench_->is_visible(Part::panel));
    EXPECT_TRUE(panel_->tab("terminal")->isChecked());
}

TEST_F(PanelTest, TheCloseButtonHidesThePanelAndTheContainerIsKept) {
    add("terminal");
    click("terminal");
    QWidget* widget = panel_->container_widget("terminal");
    panel_->close_button()->click();
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(panel_->active_container(), "terminal");

    workbench_->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_EQ(panel_->container_widget("terminal"), widget);
    ASSERT_NE(widget, nullptr);
    EXPECT_TRUE(widget->isVisible());
    EXPECT_EQ(created_["terminal"], 1);
}

TEST_F(PanelTest, ShowingThePanelAnotherWayShowsTheLastContainerOrTheFirst) {
    add("terminal", 0);
    add("output", 1);
    workbench_->set_visible(Part::panel, true);
    EXPECT_EQ(panel_->active_container(), "terminal");
    click("output");
    workbench_->set_visible(Part::panel, false);
    workbench_->set_visible(Part::panel, true);
    EXPECT_EQ(panel_->active_container(), "output");
    ASSERT_NE(panel_->container_widget("output"), nullptr);
    EXPECT_TRUE(panel_->container_widget("output")->isVisible());
}

TEST_F(PanelTest, ThePanelOpensEmptyWhenThereAreNoContainers) {
    workbench_->set_visible(Part::panel, true);
    EXPECT_TRUE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(panel_->active_container(), "");
    EXPECT_TRUE(panel_->tab_ids().empty());
    EXPECT_NE(panel_->close_button(), nullptr);
}

TEST_F(PanelTest, TheActiveContainerIsAPropertyWithASignal) {
    add("terminal");
    add("output", 1);
    QSignalSpy changed(panel_.get(), &Panel::active_container_changed);
    click("terminal");
    ASSERT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).toString(), "terminal");
    click("terminal");
    EXPECT_EQ(changed.count(), 1);
    click("output");
    ASSERT_EQ(changed.count(), 2);
    EXPECT_EQ(changed.at(1).at(0).toString(), "output");
    panel_->activate("nothing");
    EXPECT_EQ(changed.count(), 2);
}

TEST_F(PanelTest, RemovingTheActiveContainerShowsAnotherOrHidesThePanel) {
    add("terminal", 0);
    add("output", 1);
    click("output");
    QPointer<QWidget> widget = panel_->container_widget("output");

    registry_.remove("output");
    QApplication::processEvents();
    EXPECT_EQ(widget.data(), nullptr);
    EXPECT_EQ(panel_->active_container(), "terminal");
    EXPECT_TRUE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(panel_->tab_ids(), (std::vector<std::string>{"terminal"}));

    registry_.remove("terminal");
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
    EXPECT_EQ(panel_->active_container(), "");
}

TEST_F(PanelTest, TheMaximizeButtonMaximizesAndRestoresThePanel) {
    add("terminal");
    click("terminal");
    EXPECT_EQ(panel_->maximize_button()->toolTip(), "Maximize Panel Size");

    panel_->maximize_button()->click();
    QApplication::processEvents();
    EXPECT_TRUE(workbench_->is_maximized(Part::panel));
    EXPECT_EQ(workbench_->panel()->height(), workbench_->height());
    EXPECT_EQ(panel_->maximize_button()->toolTip(), "Restore Panel Size");

    panel_->maximize_button()->click();
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));
    EXPECT_LT(workbench_->panel()->height(), workbench_->height());
    EXPECT_EQ(panel_->maximize_button()->toolTip(), "Maximize Panel Size");
}

TEST_F(PanelTest, ClosingAMaximizedPanelRestoresTheEditorArea) {
    add("terminal");
    click("terminal");
    panel_->maximize_button()->click();
    panel_->close_button()->click();
    QApplication::processEvents();
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
    EXPECT_FALSE(workbench_->is_maximized(Part::panel));
    EXPECT_TRUE(workbench_->is_visible(Part::editor));
}

TEST_F(PanelTest, TheHeaderButtonsAreAccessible) {
    QAccessible::setActive(true);
    add("terminal");
    click("terminal");
    const auto* close = QAccessible::queryAccessibleInterface(panel_->close_button());
    const auto* maximize = QAccessible::queryAccessibleInterface(panel_->maximize_button());
    const auto* tab = QAccessible::queryAccessibleInterface(panel_->tab("terminal"));
    ASSERT_NE(close, nullptr);
    ASSERT_NE(maximize, nullptr);
    ASSERT_NE(tab, nullptr);
    EXPECT_EQ(close->text(QAccessible::Name), "Close Panel");
    EXPECT_EQ(maximize->text(QAccessible::Name), "Maximize Panel Size");
    EXPECT_EQ(tab->text(QAccessible::Name), "Title of terminal");
    EXPECT_TRUE(tab->state().checked);
}

TEST_F(PanelTest, TheActiveTabIsUnderlinedInTheThemeColors) {
    add("terminal");
    add("output", 1);
    click("terminal");
    colors_.set_colors({{"panel.background", QColor("#202020")},
                        {"panelTitle.activeBorder", QColor("#ff8800")},
                        {"panelTitle.activeForeground", QColor("#ffffff")},
                        {"panelTitle.inactiveForeground", QColor("#888888")}});
    QApplication::processEvents();

    const QImage image = workbench_->panel()->grab().toImage();
    const QPoint terminal = panel_->tab("terminal")->mapTo(workbench_->panel(), QPoint(0, 0));
    const QPoint output = panel_->tab("output")->mapTo(workbench_->panel(), QPoint(0, 0));
    const int underline = Panel::header_height - 1;
    EXPECT_EQ(image.pixelColor(terminal + QPoint(panel_->tab("terminal")->width() / 2, underline)),
              QColor("#ff8800"));
    EXPECT_EQ(image.pixelColor(output + QPoint(panel_->tab("output")->width() / 2, underline)),
              QColor("#202020"));

    // Text is antialiased, so look for the pixel nearest to the color: how far it is, per channel.
    const auto distance = [&](const QWidget* tab, const QColor& color) {
        const QImage tab_image =
            image.copy(QRect(tab->mapTo(workbench_->panel(), QPoint(0, 0)), tab->size()));
        int nearest = 255;
        for (int y = 0; y < tab_image.height() - 2; ++y) {
            for (int x = 0; x < tab_image.width(); ++x) {
                const QColor pixel = tab_image.pixelColor(x, y);
                nearest = std::min(nearest, std::max({std::abs(pixel.red() - color.red()),
                                                      std::abs(pixel.green() - color.green()),
                                                      std::abs(pixel.blue() - color.blue())}));
            }
        }
        return nearest;
    };
    EXPECT_LE(distance(panel_->tab("terminal"), QColor("#ffffff")), 40);
    EXPECT_LE(distance(panel_->tab("output"), QColor("#888888")), 40);
    EXPECT_GT(distance(panel_->tab("output"), QColor("#ffffff")), 100);
}

TEST_F(PanelTest, RendersThePanel) {
    add("problems", 0);
    add("output", 1);
    add("terminal", 2);
    click("output");
    const QImage image = workbench_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/panel.png");
    }
}

TEST_F(PanelTest, AContainerOfAnotherLocationIsNotShown) {
    add("explorer", 0, ViewContainerLocation::sidebar);
    panel_->activate("explorer");
    QApplication::processEvents();
    EXPECT_EQ(panel_->active_container(), "");
    EXPECT_EQ(panel_->container_widget("explorer"), nullptr);
    EXPECT_EQ(created_["explorer"], 0);
    EXPECT_FALSE(workbench_->is_visible(Part::panel));
}
