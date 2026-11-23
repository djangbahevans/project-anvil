#include "anvil/ui/title_bar.hpp"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QEnterEvent>
#include <QImage>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using anvil::ui::TitleBar;
using anvil::ui::WorkbenchColors;

namespace {

class TitleBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        colors_.set_colors({{"titleBar.activeBackground", QColor("#181818")},
                            {"titleBar.activeForeground", QColor("#cccccc")},
                            {"titleBar.inactiveBackground", QColor("#1f1f1f")},
                            {"titleBar.inactiveForeground", QColor("#9d9d9d")},
                            {"toolbar.hoverBackground", QColor("#5a5d5e")},
                            {"commandCenter.border", QColor("#2e3031")},
                            {"commandCenter.activeBorder", QColor("#333536")},
                            {"commandCenter.inactiveBorder", QColor("#262728")},
                            {"commandCenter.activeBackground", QColor("#303132")},
                            {"commandCenter.background", QColor("#191a1b")},
                            {"commandCenter.foreground", QColor("#bfbfbf")}});
        bar_ = std::make_unique<TitleBar>(colors_);
        bar_->resize(900, TitleBar::bar_height);
        bar_->show();
        QApplication::processEvents();
    }

    QImage image() const {
        return bar_->grab().toImage();
    }
    QColor pixel(QPoint at) const {
        return image().pixelColor(at);
    }
    void press(QWidget* widget, QPoint at, Qt::MouseButton button = Qt::LeftButton) {
        QMouseEvent event(QEvent::MouseButtonPress, at, widget->mapToGlobal(at), button, button,
                          Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
    QPoint empty_area() const {
        return QPoint(bar_->width() / 2, 4);
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<TitleBar> bar_;
};

TEST_F(TitleBarTest, IsAsHighAsVSCodesAndTheButtonsSitAtItsRightEdge) {
    EXPECT_EQ(bar_->height(), 35);
    EXPECT_EQ(bar_->close_button()->geometry().right(), bar_->width() - 1);
    EXPECT_EQ(bar_->close_button()->width(), 46);
    EXPECT_EQ(bar_->maximize_button()->geometry().right() + 1, bar_->close_button()->x());
    EXPECT_EQ(bar_->minimize_button()->geometry().right() + 1, bar_->maximize_button()->x());
}

TEST_F(TitleBarTest, TheButtonsSayWhatTheyAsk) {
    QSignalSpy minimize(bar_.get(), &TitleBar::minimize_requested);
    QSignalSpy toggle(bar_.get(), &TitleBar::toggle_maximized_requested);
    QSignalSpy close(bar_.get(), &TitleBar::close_requested);
    bar_->minimize_button()->click();
    EXPECT_EQ(minimize.count(), 1);
    EXPECT_EQ(toggle.count() + close.count(), 0);
    bar_->maximize_button()->click();
    EXPECT_EQ(toggle.count(), 1);
    bar_->close_button()->click();
    EXPECT_EQ(close.count(), 1);
}

TEST_F(TitleBarTest, AnEmptyAreaAsksToMoveTheWindowAndADoubleClickToMaximizeIt) {
    QSignalSpy move(bar_.get(), &TitleBar::move_requested);
    QSignalSpy toggle(bar_.get(), &TitleBar::toggle_maximized_requested);

    press(bar_.get(), empty_area());
    EXPECT_EQ(move.count(), 1);
    press(bar_.get(), empty_area(), Qt::RightButton);
    EXPECT_EQ(move.count(), 1);

    QMouseEvent double_click(QEvent::MouseButtonDblClick, empty_area(),
                             bar_->mapToGlobal(empty_area()), Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
    QApplication::sendEvent(bar_.get(), &double_click);
    EXPECT_EQ(toggle.count(), 1);
}

TEST_F(TitleBarTest, APressOnAButtonDoesNotMoveTheWindow) {
    QSignalSpy move(bar_.get(), &TitleBar::move_requested);
    QTest::mouseClick(bar_->minimize_button(), Qt::LeftButton);
    EXPECT_EQ(move.count(), 0);
}

TEST_F(TitleBarTest, TheButtonsHaveTheNamesAndRolesOfButtons) {
    const struct {
        QAbstractButton* button;
        QString name;
    } expected[] = {{bar_->minimize_button(), "Minimize"},
                    {bar_->maximize_button(), "Maximize"},
                    {bar_->close_button(), "Close"}};
    for (const auto& [button, name] : expected) {
        EXPECT_EQ(button->accessibleName(), name);
        EXPECT_EQ(button->toolTip(), name);
        EXPECT_EQ(button->focusPolicy(), Qt::NoFocus);
        QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(button);
        ASSERT_NE(interface, nullptr);
        EXPECT_EQ(interface->role(), QAccessible::Button);
        EXPECT_EQ(interface->text(QAccessible::Name), name);
    }
    EXPECT_EQ(bar_->accessibleName(), "Title bar");
}

TEST_F(TitleBarTest, MaximizingShowsTheRestoreButton) {
    const QImage maximize = image();
    bar_->set_maximized(true);
    EXPECT_EQ(bar_->maximize_button()->accessibleName(), "Restore");
    EXPECT_EQ(bar_->maximize_button()->toolTip(), "Restore");
    EXPECT_NE(image(), maximize);
    bar_->set_maximized(false);
    EXPECT_EQ(bar_->maximize_button()->accessibleName(), "Maximize");
    EXPECT_EQ(image(), maximize);
}

TEST_F(TitleBarTest, PaintsTheActiveAndInactiveBackgrounds) {
    const QPoint at = empty_area();
    EXPECT_EQ(pixel(at), QColor("#181818"));
    bar_->set_active(false);
    EXPECT_EQ(pixel(at), QColor("#1f1f1f"));
    bar_->set_active(true);
    EXPECT_EQ(pixel(at), QColor("#181818"));
}

TEST_F(TitleBarTest, FollowsTheColorsOfTheTheme) {
    colors_.set_colors({{"titleBar.activeBackground", QColor("#ff0000")}});
    EXPECT_EQ(pixel(empty_area()), QColor("#ff0000"));
}

TEST_F(TitleBarTest, DrawsTheBorderBelowOnlyWhenTheThemeSetsIt) {
    const QPoint bottom(empty_area().x(), bar_->height() - 1);
    EXPECT_EQ(pixel(bottom), QColor("#181818"));
    colors_.set_colors(
        {{"titleBar.activeBackground", QColor("#181818")}, {"titleBar.border", QColor("#2b2b2b")}});
    EXPECT_EQ(pixel(bottom), QColor("#2b2b2b"));
}

TEST_F(TitleBarTest, DrawsTheTitleInTheMiddleAndAnInactiveTitleDimmer) {
    const QImage empty = image();
    bar_->set_title("a.txt — Anvil");
    const QImage titled = image();
    EXPECT_NE(titled, empty);
    EXPECT_EQ(bar_->title(), "a.txt — Anvil");

    // Only the middle of the bar changed.
    const QRect middle(bar_->width() / 2 - 100, 0, 200, bar_->height());
    for (int y = 0; y < titled.height(); ++y) {
        for (int x = 0; x < titled.width(); ++x) {
            if (!middle.contains(x, y)) {
                ASSERT_EQ(titled.pixel(x, y), empty.pixel(x, y)) << x << "," << y;
            }
        }
    }

    const auto brightest = [&](const QImage& picture) {
        int best = 0;
        for (int y = 0; y < picture.height(); ++y) {
            for (int x = middle.left(); x <= middle.right(); ++x) {
                best = std::max(best, picture.pixelColor(x, y).lightness());
            }
        }
        return best;
    };
    const int active = brightest(titled);
    bar_->set_active(false);
    EXPECT_LT(brightest(image()), active);
}

TEST_F(TitleBarTest, TheMenuBarFollowsTheIconAndThePlaceOfTheTitleMovesAfterIt) {
    auto* menu_bar = new QMenuBar;
    menu_bar->addMenu("File");
    menu_bar->addMenu("Edit");
    bar_->set_menu_bar(menu_bar);
    QApplication::processEvents();

    EXPECT_EQ(menu_bar->parentWidget(), bar_.get());
    EXPECT_GE(menu_bar->x(), 36);
    EXPECT_LT(menu_bar->geometry().right(), bar_->minimize_button()->x());
    // The empty area to the right of the menus still moves the window.
    QSignalSpy move(bar_.get(), &TitleBar::move_requested);
    press(bar_.get(), QPoint(menu_bar->geometry().right() + 40, 4));
    EXPECT_EQ(move.count(), 1);
}

TEST_F(TitleBarTest, TheMenuBarTakesTheBackgroundAndTheTextOfTheTitleBar) {
    auto* menu_bar = new QMenuBar;
    menu_bar->addMenu("File");
    bar_->set_menu_bar(menu_bar);
    QApplication::processEvents();

    const QPoint at = menu_bar->geometry().topRight() + QPoint(-2, 2);
    EXPECT_EQ(pixel(at), QColor("#181818"));
    EXPECT_TRUE(menu_bar->styleSheet().contains("#cccccc")) << menu_bar->styleSheet().toStdString();
    bar_->set_active(false);
    EXPECT_TRUE(menu_bar->styleSheet().contains("#9d9d9d"));
    EXPECT_EQ(pixel(at), QColor("#1f1f1f"));
}

TEST_F(TitleBarTest, TheCloseButtonTurnsRedUnderThePointerAndTheOthersFollowTheTheme) {
    const auto corner = [&](QAbstractButton* button) {
        return pixel(button->geometry().topLeft() + QPoint(2, 2));
    };
    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QEvent leave(QEvent::Leave);

    EXPECT_EQ(corner(bar_->close_button()), QColor("#181818"));
    QApplication::sendEvent(bar_->close_button(), &enter);
    EXPECT_EQ(corner(bar_->close_button()), QColor("#e81123"));
    QApplication::sendEvent(bar_->close_button(), &leave);
    EXPECT_EQ(corner(bar_->close_button()), QColor("#181818"));

    QApplication::sendEvent(bar_->minimize_button(), &enter);
    EXPECT_EQ(corner(bar_->minimize_button()), QColor("#5a5d5e"));
    QApplication::sendEvent(bar_->minimize_button(), &leave);
}

TEST_F(TitleBarTest, TheCommandCenterIsHiddenUntilItIsAskedFor) {
    EXPECT_FALSE(bar_->command_center_visible());
    EXPECT_FALSE(bar_->command_center()->isVisible());
    bar_->set_command_center_visible(true);
    EXPECT_TRUE(bar_->command_center_visible());
    EXPECT_TRUE(bar_->command_center()->isVisible());
    bar_->set_command_center_visible(false);
    EXPECT_FALSE(bar_->command_center()->isVisible());
}

TEST_F(TitleBarTest, TheCommandCenterIsInTheMiddleOfTheBarAndNoWiderThanItNeedsToBe) {
    bar_->set_command_center_text("Anvil");
    bar_->set_command_center_visible(true);
    const QRect box = bar_->command_center()->geometry();
    EXPECT_EQ(box.height(), 24);
    EXPECT_EQ(box.y(), (bar_->height() - 24) / 2);
    EXPECT_LE(box.width(), 450);
    EXPECT_LE(std::abs(box.center().x() - bar_->width() / 2), 1);

    bar_->resize(500, TitleBar::bar_height);
    QApplication::processEvents();
    const QRect narrow = bar_->command_center()->geometry();
    EXPECT_EQ(narrow.width(), 200);
    EXPECT_LE(std::abs(narrow.center().x() - bar_->width() / 2), 1);
}

TEST_F(TitleBarTest, TheCommandCenterGivesWayToTheMenuBarAndLeavesWhenThereIsNoRoom) {
    auto* menu_bar = new QMenuBar;
    for (const char* name :
         {"File", "Edit", "Selection", "View", "Go", "Run", "Terminal", "Help"}) {
        menu_bar->addMenu(name);
    }
    bar_->set_menu_bar(menu_bar);
    bar_->set_command_center_text("Anvil");
    bar_->set_command_center_visible(true);
    bar_->resize(900, TitleBar::bar_height);
    QApplication::processEvents();
    ASSERT_TRUE(bar_->command_center()->isVisible());
    EXPECT_GT(bar_->command_center()->x(), menu_bar->geometry().right());
    EXPECT_LT(bar_->command_center()->geometry().right(), bar_->minimize_button()->x());

    bar_->resize(520, TitleBar::bar_height);
    QApplication::processEvents();
    EXPECT_FALSE(bar_->command_center()->isVisible());

    bar_->resize(900, TitleBar::bar_height);
    QApplication::processEvents();
    EXPECT_TRUE(bar_->command_center()->isVisible());
}

TEST_F(TitleBarTest, ClickingTheCommandCenterAsksForTheCommandPalette) {
    bar_->set_command_center_visible(true);
    QSignalSpy asked(bar_.get(), &TitleBar::command_center_requested);
    QSignalSpy moved(bar_.get(), &TitleBar::move_requested);
    QTest::mouseClick(bar_->command_center(), Qt::LeftButton);
    EXPECT_EQ(asked.count(), 1);
    EXPECT_EQ(moved.count(), 0);
}

TEST_F(TitleBarTest, TheCommandCenterTakesThePlaceOfTheTitle) {
    bar_->set_title("a.txt — Anvil");
    const QImage drawn = image();
    bar_->set_title("");
    EXPECT_NE(image(), drawn);

    // A title longer than the box reaches outside it, and is not drawn there.
    bar_->set_command_center_visible(true);
    const QRect box = bar_->command_center()->geometry();
    bar_->set_title(QString(90, 'x'));
    const QImage first = image();
    bar_->set_title(QString(90, 'W'));
    const QImage second = image();
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            if (!box.contains(x, y)) {
                ASSERT_EQ(first.pixel(x, y), second.pixel(x, y)) << x << "," << y;
            }
        }
    }
}

TEST_F(TitleBarTest, TheCommandCenterShowsItsTextWithASearchIcon) {
    bar_->set_command_center_visible(true);
    bar_->set_command_center_text("");
    const QImage empty = bar_->command_center()->grab().toImage();
    bar_->set_command_center_text("project-anvil");
    const QImage named = bar_->command_center()->grab().toImage();
    EXPECT_NE(named, empty);
    EXPECT_EQ(bar_->command_center()->accessibleName(), "Command center");
    EXPECT_EQ(bar_->command_center()->accessibleDescription(), "project-anvil");
    EXPECT_EQ(bar_->command_center()->focusPolicy(), Qt::NoFocus);
    // Even the empty box has the icon.
    int bright = 0;
    for (int x = 4; x < empty.width() - 4; ++x) {
        for (int y = 4; y < empty.height() - 4; ++y) {
            bright = std::max(bright, empty.pixelColor(x, y).lightness());
        }
    }
    EXPECT_GT(bright, 100);
}

TEST_F(TitleBarTest, TheCommandCenterHasTheColorsOfItsKeysInEachState) {
    bar_->set_command_center_visible(true);
    QWidget* box = bar_->command_center();
    const auto edge = [&] { return box->grab().toImage().pixelColor(box->width() / 2, 0); };
    const auto inside = [&] { return box->grab().toImage().pixelColor(box->width() - 6, 12); };
    EXPECT_EQ(edge(), QColor("#2e3031"));
    EXPECT_EQ(inside(), QColor("#191a1b"));

    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(box, &enter);
    EXPECT_EQ(edge(), QColor("#333536"));
    EXPECT_EQ(inside(), QColor("#303132"));
    QApplication::sendEvent(box, &leave);

    bar_->set_active(false);
    EXPECT_EQ(edge(), QColor("#262728"));
}

TEST_F(TitleBarTest, TheLayoutControlsAreHiddenUntilTheyAreAskedFor) {
    EXPECT_FALSE(bar_->layout_controls_visible());
    EXPECT_FALSE(bar_->sidebar_button()->isVisible());
    EXPECT_FALSE(bar_->panel_button()->isVisible());
    bar_->set_layout_controls_visible(true);
    QApplication::processEvents();
    EXPECT_TRUE(bar_->sidebar_button()->isVisible());
    EXPECT_TRUE(bar_->panel_button()->isVisible());
    EXPECT_LT(bar_->sidebar_button()->x(), bar_->panel_button()->x());
    EXPECT_LT(bar_->panel_button()->geometry().right(), bar_->minimize_button()->x());
    EXPECT_EQ(bar_->sidebar_button()->width(), 28);

    bar_->set_layout_controls_visible(false);
    QApplication::processEvents();
    EXPECT_FALSE(bar_->sidebar_button()->isVisible());
}

TEST_F(TitleBarTest, TheLayoutButtonsAskForTheChangeAndShowWhetherThePartIsShown) {
    bar_->set_layout_controls_visible(true);
    QApplication::processEvents();
    QSignalSpy sidebar(bar_.get(), &TitleBar::toggle_sidebar_requested);
    QSignalSpy panel(bar_.get(), &TitleBar::toggle_panel_requested);

    EXPECT_FALSE(bar_->sidebar_button()->isChecked());
    const QImage hidden = bar_->sidebar_button()->grab().toImage();
    bar_->sidebar_button()->click();
    EXPECT_EQ(sidebar.count(), 1);
    // The window decides, so a click alone changes nothing.
    EXPECT_FALSE(bar_->sidebar_button()->isChecked());
    EXPECT_EQ(bar_->sidebar_button()->grab().toImage(), hidden);

    bar_->set_sidebar_shown(true);
    EXPECT_TRUE(bar_->sidebar_button()->isChecked());
    EXPECT_NE(bar_->sidebar_button()->grab().toImage(), hidden);
    bar_->set_sidebar_shown(false);
    EXPECT_EQ(bar_->sidebar_button()->grab().toImage(), hidden);

    const QImage panel_hidden = bar_->panel_button()->grab().toImage();
    bar_->panel_button()->click();
    EXPECT_EQ(panel.count(), 1);
    bar_->set_panel_shown(true);
    EXPECT_TRUE(bar_->panel_button()->isChecked());
    EXPECT_NE(bar_->panel_button()->grab().toImage(), panel_hidden);
}

TEST_F(TitleBarTest, TheLayoutButtonsHaveNamesAndDoNotTakeTheFocus) {
    for (QAbstractButton* button : {bar_->sidebar_button(), bar_->panel_button()}) {
        EXPECT_EQ(button->focusPolicy(), Qt::NoFocus);
        EXPECT_FALSE(button->accessibleName().isEmpty());
        QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(button);
        ASSERT_NE(interface, nullptr);
        EXPECT_EQ(interface->text(QAccessible::Name), button->accessibleName());
    }
    EXPECT_EQ(bar_->sidebar_button()->accessibleName(), "Toggle Primary Side Bar");
    EXPECT_EQ(bar_->panel_button()->accessibleName(), "Toggle Panel");
}

TEST_F(TitleBarTest, ALayoutButtonUnderThePointerHasTheHoverBackground) {
    bar_->set_layout_controls_visible(true);
    QApplication::processEvents();
    QWidget* button = bar_->sidebar_button();
    const auto corner = [&] { return pixel(button->geometry().topLeft() + QPoint(5, 14)); };
    EXPECT_EQ(corner(), QColor("#181818"));
    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QApplication::sendEvent(button, &enter);
    EXPECT_EQ(corner(), QColor("#5a5d5e"));
}

TEST_F(TitleBarTest, TheCommandCenterGetsMoreRoomWithoutTheLayoutControls) {
    bar_->resize(420, TitleBar::bar_height);
    bar_->set_command_center_visible(true);
    QApplication::processEvents();
    const int without = bar_->command_center()->width();
    bar_->set_layout_controls_visible(true);
    QApplication::processEvents();
    EXPECT_LT(bar_->command_center()->geometry().right(), bar_->sidebar_button()->x());
    EXPECT_LE(bar_->command_center()->width(), without);
}

TEST_F(TitleBarTest, Screenshot) {
    auto* menu_bar = new QMenuBar;
    menu_bar->addMenu("File");
    menu_bar->addMenu("Edit");
    menu_bar->addMenu("Selection");
    bar_->set_menu_bar(menu_bar);
    bar_->set_title("a.txt — Anvil");
    bar_->set_maximized(true);
    bar_->set_command_center_text("Anvil");
    bar_->set_command_center_visible(true);
    bar_->set_layout_controls_visible(true);
    bar_->set_sidebar_shown(true);
    QApplication::processEvents();
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        bar_->grab().save(QString("%1/title_bar.png").arg(directory));
    }
}

} // namespace
