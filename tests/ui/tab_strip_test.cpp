#include "accessibility_recorder.hpp"
#include "anvil/ui/tab_strip.hpp"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QHelpEvent>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <QWheelEvent>
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

class TabStripTest : public ::testing::Test {
protected:
    void SetUp() override {
        colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#101010")},
                            {"tab.activeBackground", QColor("#202020")},
                            {"tab.inactiveBackground", QColor("#303030")},
                            {"tab.hoverBackground", QColor("#404040")},
                            {"tab.activeForeground", QColor("#ffffff")},
                            {"tab.inactiveForeground", QColor("#aaaaaa")},
                            {"tab.activeBorderTop", QColor("#3994bc")},
                            {"tab.border", QColor("#0000ff")},
                            {"toolbar.hoverBackground", QColor("#606060")}});
        strip_ = std::make_unique<TabStrip>(colors_);
        strip_->resize(600, TabStrip::height_pixels);
        strip_->show();
        QApplication::processEvents();
    }

    void add(const QString& title, bool dirty = false) {
        strip_->add_tab(
            {.title = title, .tooltip = "/path/" + title, .dirty = dirty, .has_path = true});
    }
    void add_many(int count) {
        for (int i = 0; i < count; ++i) {
            add("file" + QString::number(i) + ".txt");
        }
    }

    void send(QEvent::Type type, const QPoint& position, Qt::MouseButton button,
              Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, strip_->mapToGlobal(position), button, buttons, {});
        QApplication::sendEvent(strip_.get(), &event);
    }
    void press(const QPoint& position, Qt::MouseButton button = Qt::LeftButton) {
        send(QEvent::MouseButtonPress, position, button, button);
    }
    void release(const QPoint& position, Qt::MouseButton button = Qt::LeftButton) {
        send(QEvent::MouseButtonRelease, position, button, Qt::NoButton);
    }
    void move(const QPoint& position, Qt::MouseButtons buttons = Qt::NoButton) {
        send(QEvent::MouseMove, position, Qt::NoButton, buttons);
    }
    void click(const QPoint& position, Qt::MouseButton button = Qt::LeftButton) {
        press(position, button);
        release(position, button);
    }

    QPoint body(int index) const {
        return strip_->tab_rect(index).center() - QPoint(10, 0);
    }
    QPoint button(int index) const {
        return strip_->close_button_rect(index).center();
    }

    QImage grab() {
        QApplication::processEvents();
        return strip_->grab().toImage();
    }
    QColor pixel(const QImage& image, const QPoint& at) const {
        return QColor(image.pixel(at));
    }

    std::vector<QString> titles() const {
        std::vector<QString> result;
        for (int i = 0; i < strip_->count(); ++i) {
            result.push_back(strip_->tab(i).title);
        }
        return result;
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<TabStrip> strip_;
};

} // namespace

TEST_F(TabStripTest, IsAsHighAsVsCodesTabStripAndUsesItsFontSize) {
    EXPECT_EQ(strip_->height(), 35);
    EXPECT_EQ(strip_->font().pixelSize(), 13);
    EXPECT_EQ(strip_->accessibleName(), "Tabs");
}

// --- accessibility ---

namespace {

class TabStripAccessibilityTest : public TabStripTest {
protected:
    void SetUp() override {
        TabStripTest::SetUp();
        recorder_ = std::make_unique<AccessibilityRecorder>();
    }
    void TearDown() override {
        recorder_.reset();
    }

    QAccessibleInterface* strip_interface() {
        return QAccessible::queryAccessibleInterface(strip_.get());
    }

    std::vector<QString> names() {
        std::vector<QString> result;
        QAccessibleInterface* strip = strip_interface();
        for (int i = 0; i < strip->childCount(); ++i) {
            result.push_back(strip->child(i)->text(QAccessible::Name));
        }
        return result;
    }

    std::unique_ptr<AccessibilityRecorder> recorder_;
};

} // namespace

TEST_F(TabStripAccessibilityTest, TheStripIsAPageTabListWithAPageTabForEachTab) {
    add("a.txt");
    add("b.txt");
    QAccessibleInterface* strip = strip_interface();
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->role(), QAccessible::PageTabList);
    EXPECT_EQ(strip->text(QAccessible::Name), "Tabs");
    ASSERT_EQ(strip->childCount(), 2);
    for (int i = 0; i < 2; ++i) {
        QAccessibleInterface* tab = strip->child(i);
        EXPECT_EQ(tab->role(), QAccessible::PageTab);
        EXPECT_EQ(tab->parent(), strip);
        EXPECT_EQ(strip->indexOfChild(tab), i);
    }
    EXPECT_EQ(names(), (std::vector<QString>{"a.txt", "b.txt"}));
    EXPECT_EQ(strip->child(1)->text(QAccessible::Description), "/path/b.txt");
}

TEST_F(TabStripAccessibilityTest, TheCurrentTabIsSelectedAndTheStateFollowsTheSelection) {
    add("a");
    add("b");
    QAccessibleInterface* strip = strip_interface();
    EXPECT_TRUE(strip->child(0)->state().selected);
    EXPECT_FALSE(strip->child(1)->state().selected);
    EXPECT_TRUE(strip->child(1)->state().selectable);
    strip_->set_current_index(1);
    EXPECT_FALSE(strip->child(0)->state().selected);
    EXPECT_TRUE(strip->child(1)->state().selected);
}

TEST_F(TabStripAccessibilityTest, ADirtyTabSaysSoInItsDescription) {
    add("a", true);
    strip_->add_tab({.title = "b", .tooltip = {}, .dirty = true});
    const auto* strip = strip_interface();
    EXPECT_EQ(strip->child(0)->text(QAccessible::Description), "/path/a\nUnsaved changes");
    EXPECT_EQ(strip->child(1)->text(QAccessible::Description), "Unsaved changes");
}

TEST_F(TabStripAccessibilityTest, ATabsRectangleIsWhereItIsPainted) {
    add("a");
    add("b");
    const QRect painted = strip_->tab_rect(1);
    QAccessibleInterface* tab = strip_interface()->child(1);
    EXPECT_EQ(tab->rect().size(), painted.size());
    EXPECT_EQ(tab->rect().topLeft(), strip_->mapToGlobal(painted.topLeft()));
    EXPECT_EQ(strip_interface()->childAt(tab->rect().center().x(), tab->rect().center().y()), tab);
}

TEST_F(TabStripAccessibilityTest, PressingATabSelectsItAndClosingAsksToCloseIt) {
    add("a");
    add("b");
    QSignalSpy selected(strip_.get(), &TabStrip::current_changed);
    QSignalSpy closes(strip_.get(), &TabStrip::close_requested);
    QAccessibleActionInterface* actions = strip_interface()->child(1)->actionInterface();
    ASSERT_NE(actions, nullptr);
    EXPECT_EQ(actions->actionNames(),
              (QStringList{QAccessibleActionInterface::pressAction(), TabStrip::close_action()}));

    actions->doAction(QAccessibleActionInterface::pressAction());
    EXPECT_EQ(strip_->current_index(), 1);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).toInt(), 1);
    actions->doAction(QAccessibleActionInterface::pressAction());
    EXPECT_EQ(selected.count(), 1);

    actions->doAction(TabStrip::close_action());
    ASSERT_EQ(closes.count(), 1);
    EXPECT_EQ(closes.at(0).at(0).toInt(), 1);
    EXPECT_EQ(strip_->count(), 2);
}

TEST_F(TabStripAccessibilityTest, ATabKeepsItsInterfaceWhenTheOthersMove) {
    add("a");
    add("b");
    add("c");
    QAccessibleInterface* b = strip_interface()->child(1);
    const QAccessible::Id id = QAccessible::uniqueId(b);
    strip_->move_tab(0, 2);
    EXPECT_EQ(names(), (std::vector<QString>{"b", "c", "a"}));
    EXPECT_EQ(QAccessible::accessibleInterface(id), strip_interface()->child(0));
    EXPECT_EQ(strip_interface()->child(0)->text(QAccessible::Name), "b");
}

TEST_F(TabStripAccessibilityTest, TheInterfaceOfAClosedTabIsDropped) {
    add("a");
    add("b");
    const QAccessible::Id id = QAccessible::uniqueId(strip_interface()->child(1));
    strip_->remove_tab(1);
    EXPECT_EQ(names(), (std::vector<QString>{"a"}));
    EXPECT_EQ(QAccessible::accessibleInterface(id), nullptr);
}

TEST_F(TabStripAccessibilityTest, RetitlingATabSignalsANameChangeAndOnlyThat) {
    add("a");
    add("b");
    recorder_->clear();
    strip_->set_tab(1,
                    {.title = "renamed", .tooltip = "/path/b", .dirty = false, .has_path = true});
    EXPECT_TRUE(recorder_->contains(QAccessible::NameChanged, strip_.get(), 1));
    EXPECT_EQ(recorder_->events().size(), 1U);

    recorder_->clear();
    strip_->set_tab(1,
                    {.title = "renamed", .tooltip = "/path/b", .dirty = false, .has_path = true});
    strip_->set_tab(1, {.title = "renamed", .tooltip = "/path/b", .dirty = true, .has_path = true});
    EXPECT_TRUE(recorder_->events().empty());
}

TEST_F(TabStripAccessibilityTest, ChangingTheSelectionSignalsAStateChangeForBothTabs) {
    add("a");
    add("b");
    recorder_->clear();
    strip_->set_current_index(1);
    EXPECT_TRUE(recorder_->contains(QAccessible::StateChanged, strip_.get(), 0));
    EXPECT_TRUE(recorder_->contains(QAccessible::StateChanged, strip_.get(), 1));
    recorder_->clear();
    strip_->set_current_index(1);
    EXPECT_TRUE(recorder_->events().empty());
}

TEST_F(TabStripTest, ATabIsAtLeast120PixelsWideAndWideEnoughForItsTitle) {
    add("a");
    add("a title that is much longer than the minimum width of a tab");
    EXPECT_EQ(strip_->tab_rect(0).width(), 120);
    const int title = QFontMetrics(strip_->font()).horizontalAdvance(strip_->tab(1).title);
    EXPECT_EQ(strip_->tab_rect(1).width(),
              TabStrip::title_padding + title + 1 + TabStrip::action_width);
    EXPECT_GT(strip_->tab_rect(1).width(), 120);
    EXPECT_EQ(strip_->tab_rect(1).left(), strip_->tab_rect(0).right() + 1);
    EXPECT_EQ(strip_->tab_rect(0).height(), 35);
}

TEST_F(TabStripTest, TheCloseButtonIsAtTheRightEndOfItsTab) {
    add("a");
    const QRect tab = strip_->tab_rect(0);
    const QRect close = strip_->close_button_rect(0);
    EXPECT_EQ(close.size(), QSize(16, 16));
    EXPECT_TRUE(tab.contains(close));
    EXPECT_GT(close.left(), tab.center().x());
    EXPECT_NEAR(close.center().y(), tab.center().y(), 1);
    EXPECT_TRUE(strip_->tab_rect(5).isEmpty());
    EXPECT_TRUE(strip_->close_button_rect(-1).isEmpty());
}

TEST_F(TabStripTest, TheFirstTabIsSelectedAndAddingTabsDoesNotChangeTheSelection) {
    EXPECT_EQ(strip_->current_index(), -1);
    add("a");
    EXPECT_EQ(strip_->current_index(), 0);
    add("b");
    add("c");
    EXPECT_EQ(strip_->current_index(), 0);
    EXPECT_EQ(strip_->count(), 3);
}

TEST_F(TabStripTest, ChangingTheModelEmitsNothing) {
    QSignalSpy current(strip_.get(), &TabStrip::current_changed);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    add("a");
    add("b");
    add("c");
    strip_->set_current_index(2);
    strip_->move_tab(2, 0);
    strip_->set_tab(0, {.title = "renamed"});
    strip_->remove_tab(1);
    EXPECT_EQ(current.count() + moved.count() + closed.count(), 0);
    EXPECT_EQ(strip_->current_index(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"renamed", "b"}));
}

TEST_F(TabStripTest, ClickingATabSelectsItAndSaysSo) {
    add_many(3);
    QSignalSpy spy(strip_.get(), &TabStrip::current_changed);
    click(body(2));
    EXPECT_EQ(strip_->current_index(), 2);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 2);
    click(body(2));
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TabStripTest, TheTabIsSelectedWhenTheButtonGoesDownNotWhenItComesUp) {
    add_many(2);
    QSignalSpy spy(strip_.get(), &TabStrip::current_changed);
    press(body(1));
    EXPECT_EQ(spy.count(), 1);
    release(body(1));
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TabStripTest, ClickingOutsideAnyTabDoesNothing) {
    add("a");
    QSignalSpy spy(strip_.get(), &TabStrip::current_changed);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    click(QPoint(500, 10));
    click(QPoint(500, 10), Qt::MiddleButton);
    EXPECT_EQ(spy.count() + closed.count(), 0);
}

TEST_F(TabStripTest, RemovingATabKeepsTheSelectedTabSelected) {
    add_many(4);
    strip_->set_current_index(2);
    strip_->remove_tab(0);
    EXPECT_EQ(strip_->current_index(), 1);
    EXPECT_EQ(strip_->tab(1).title, "file2.txt");
    strip_->remove_tab(2);
    EXPECT_EQ(strip_->current_index(), 1);
}

TEST_F(TabStripTest, RemovingTheSelectedTabSelectsTheOneThatTakesItsPlace) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->remove_tab(1);
    EXPECT_EQ(strip_->current_index(), 1);
    EXPECT_EQ(strip_->tab(1).title, "file2.txt");
    strip_->remove_tab(1);
    EXPECT_EQ(strip_->current_index(), 0);
    strip_->remove_tab(0);
    EXPECT_EQ(strip_->current_index(), -1);
    EXPECT_EQ(strip_->count(), 0);
}

TEST_F(TabStripTest, MovingATabKeepsTheSelectionOnItsTab) {
    add_many(4);
    strip_->set_current_index(1);
    strip_->move_tab(0, 3);
    EXPECT_EQ(titles(), (std::vector<QString>{"file1.txt", "file2.txt", "file3.txt", "file0.txt"}));
    EXPECT_EQ(strip_->current_index(), 0);
    strip_->move_tab(0, 2);
    EXPECT_EQ(strip_->current_index(), 2);
    strip_->move_tab(3, 0);
    EXPECT_EQ(strip_->current_index(), 3);
}

TEST_F(TabStripTest, SettingATabToWhatItAlreadyIsDoesNotRepaintOrLayOut) {
    add("a");
    add("b", false);
    QApplication::processEvents();
    PaintCounter counter;
    strip_->installEventFilter(&counter);
    const QRect before = strip_->tab_rect(1);

    for (int i = 0; i < 100; ++i) {
        strip_->set_tab(1, {.title = "b", .tooltip = "/path/b", .dirty = false, .has_path = true});
    }
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 0);
    EXPECT_EQ(strip_->tab_rect(1), before);

    strip_->set_tab(1, {.title = "b", .tooltip = "/path/b", .dirty = true, .has_path = true});
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 1);
}

TEST_F(TabStripTest, ALongerTitleMovesTheTabsAfterIt) {
    add("a");
    add("b");
    const int before = strip_->tab_rect(1).left();
    strip_->set_tab(0, {.title = "a title that is much longer than the minimum width of a tab",
                        .tooltip = {},
                        .dirty = false,
                        .has_path = false});
    EXPECT_GT(strip_->tab_rect(1).left(), before);
}

TEST_F(TabStripTest, TheSelectedTabHasItsOwnBackgroundAndALineOnTop) {
    add_many(2);
    const QImage image = grab();
    const QRect active = strip_->tab_rect(0);
    const QRect inactive = strip_->tab_rect(1);
    EXPECT_EQ(pixel(image, active.topLeft() + QPoint(5, 10)), QColor("#202020"));
    EXPECT_EQ(pixel(image, active.topLeft() + QPoint(5, 0)), QColor("#3994bc"));
    EXPECT_EQ(pixel(image, active.topLeft() + QPoint(5, 1)), QColor("#202020"));
    EXPECT_EQ(pixel(image, inactive.topLeft() + QPoint(5, 10)), QColor("#303030"));
    EXPECT_EQ(pixel(image, inactive.topLeft() + QPoint(5, 0)), QColor("#303030"));
}

TEST_F(TabStripTest, TabsAreSeparatedByABorderAndTheStripHasItsOwnBackground) {
    add_many(2);
    const QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).right(), 10)), QColor("#0000ff"));
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).right() - 1, 10)), QColor("#202020"));
    EXPECT_EQ(pixel(image, QPoint(550, 10)), QColor("#101010"));
}

TEST_F(TabStripTest, ATabUnderTheMouseHasTheHoverBackground) {
    add_many(3);
    move(body(1));
    EXPECT_EQ(strip_->hovered_tab(), 1);
    QImage image = grab();
    EXPECT_EQ(pixel(image, strip_->tab_rect(1).topLeft() + QPoint(5, 10)), QColor("#404040"));
    EXPECT_EQ(pixel(image, strip_->tab_rect(2).topLeft() + QPoint(5, 10)), QColor("#303030"));
    EXPECT_EQ(pixel(image, strip_->tab_rect(0).topLeft() + QPoint(5, 10)), QColor("#202020"));
    move(body(0));
    EXPECT_EQ(strip_->hovered_tab(), 0);
    move(QPoint(550, 10));
    EXPECT_EQ(strip_->hovered_tab(), -1);
}

TEST_F(TabStripTest, AThemeWithoutAHoverColorShowsNoHoverEffect) {
    colors_.set_colors({{"tab.activeBackground", QColor("#202020")},
                        {"tab.inactiveBackground", QColor("#303030")}});
    add_many(2);
    move(body(1));
    EXPECT_EQ(pixel(grab(), strip_->tab_rect(1).topLeft() + QPoint(5, 10)), QColor("#303030"));
}

TEST_F(TabStripTest, LeavingTheStripEndsTheHover) {
    add_many(2);
    move(body(1));
    ASSERT_EQ(strip_->hovered_tab(), 1);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(strip_.get(), &leave);
    EXPECT_EQ(strip_->hovered_tab(), -1);
}

namespace {

// Pixels that differ from `background` inside `area`.
int lit_pixels(const QImage& image, const QRect& area, const QColor& background) {
    int lit = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            lit += QColor(image.pixel(x, y)) != background;
        }
    }
    return lit;
}

} // namespace

TEST_F(TabStripTest, TheCloseButtonOfTheSelectedTabIsAlwaysThereAndAnotherTabsShowsOnHover) {
    add_many(2);
    EXPECT_GT(lit_pixels(grab(), strip_->close_button_rect(0), QColor("#202020")), 4);
    EXPECT_EQ(lit_pixels(grab(), strip_->close_button_rect(1), QColor("#303030")), 0);
    move(body(1));
    EXPECT_GT(lit_pixels(grab(), strip_->close_button_rect(1), QColor("#404040")), 4);
}

TEST_F(TabStripTest, ADirtyTabShowsADotThatBecomesTheCloseButtonUnderTheMouse) {
    add("clean");
    add("dirty", true);
    const QPoint beside = strip_->close_button_rect(1).center() + QPoint(3, 0);

    QImage image = grab();
    EXPECT_EQ(pixel(image, strip_->close_button_rect(1).center()), QColor("#aaaaaa"));
    EXPECT_EQ(pixel(image, beside), QColor("#aaaaaa"));

    move(body(1));
    image = grab();
    EXPECT_EQ(pixel(image, beside), QColor("#aaaaaa"));

    move(button(1));
    EXPECT_TRUE(strip_->hovered_tab() == 1);
    image = grab();
    EXPECT_NE(pixel(image, beside), QColor("#aaaaaa"));
    EXPECT_EQ(pixel(image, strip_->close_button_rect(1).topLeft() + QPoint(1, 8)),
              QColor("#606060"));

    move(body(1));
    image = grab();
    EXPECT_EQ(pixel(image, beside), QColor("#aaaaaa"));
}

TEST_F(TabStripTest, TheDotOfADirtySelectedTabUsesTheSelectedForeground) {
    add("dirty", true);
    const QImage image = grab();
    EXPECT_EQ(pixel(image, strip_->close_button_rect(0).center()), QColor("#ffffff"));
}

TEST_F(TabStripTest, ClickingTheCloseButtonRequestsClosingWithoutSelecting) {
    add_many(3);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    QSignalSpy current(strip_.get(), &TabStrip::current_changed);
    click(button(2));
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).toInt(), 2);
    EXPECT_EQ(current.count(), 0);
    EXPECT_EQ(strip_->current_index(), 0);
}

TEST_F(TabStripTest, TheCloseButtonOfADirtyTabCloses) {
    add("dirty", true);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    click(button(0));
    EXPECT_EQ(closed.count(), 1);
}

TEST_F(TabStripTest, PressingTheCloseButtonAndReleasingElsewhereClosesNothing) {
    add_many(2);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    press(button(1));
    release(body(1));
    press(button(1));
    release(button(0));
    EXPECT_EQ(closed.count(), 0);
}

TEST_F(TabStripTest, AMiddleClickClosesTheTabUnderTheMouseWithoutSelectingIt) {
    add_many(3);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    QSignalSpy current(strip_.get(), &TabStrip::current_changed);
    click(body(1), Qt::MiddleButton);
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).toInt(), 1);
    EXPECT_EQ(current.count(), 0);
}

TEST_F(TabStripTest, AMiddleClickPressedOnOneTabAndReleasedOnAnotherClosesNothing) {
    add_many(2);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    press(body(0), Qt::MiddleButton);
    release(body(1), Qt::MiddleButton);
    EXPECT_EQ(closed.count(), 0);
}

TEST_F(TabStripTest, ARightClickSelectsNothingAndClosesNothing) {
    add_many(2);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    QSignalSpy current(strip_.get(), &TabStrip::current_changed);
    click(body(1), Qt::RightButton);
    EXPECT_EQ(closed.count() + current.count(), 0);
}

TEST_F(TabStripTest, TheContextMenuOffersTheFiveCommands) {
    add_many(3);
    const auto menu = strip_->build_context_menu(1);
    QStringList texts;
    for (const QAction* action : menu->actions()) {
        if (!action->isSeparator()) {
            texts << action->text();
        }
    }
    EXPECT_EQ(texts, (QStringList{"Close", "Close Others", "Close to the Right", "Close Saved",
                                  "Copy Path"}));
}

TEST_F(TabStripTest, TheContextMenuActionsEmitTheirRequestsForTheTab) {
    add_many(3);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    QSignalSpy others(strip_.get(), &TabStrip::close_others_requested);
    QSignalSpy right(strip_.get(), &TabStrip::close_to_the_right_requested);
    QSignalSpy saved(strip_.get(), &TabStrip::close_saved_requested);
    QSignalSpy copy(strip_.get(), &TabStrip::copy_path_requested);
    const auto menu = strip_->build_context_menu(1);
    for (QAction* action : menu->actions()) {
        action->trigger();
    }
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).toInt(), 1);
    ASSERT_EQ(others.count(), 1);
    EXPECT_EQ(others.at(0).at(0).toInt(), 1);
    ASSERT_EQ(right.count(), 1);
    EXPECT_EQ(right.at(0).at(0).toInt(), 1);
    EXPECT_EQ(saved.count(), 1);
    ASSERT_EQ(copy.count(), 1);
    EXPECT_EQ(copy.at(0).at(0).toInt(), 1);
}

TEST_F(TabStripTest, MenuItemsThatHaveNothingToDoAreDisabled) {
    strip_->add_tab({.title = "only", .tooltip = {}, .dirty = false, .has_path = false});
    auto menu = strip_->build_context_menu(0);
    const auto enabled = [&](const QString& text) {
        for (const QAction* action : menu->actions()) {
            if (action->text() == text) {
                return action->isEnabled();
            }
        }
        ADD_FAILURE() << text.toStdString();
        return false;
    };
    EXPECT_TRUE(enabled("Close"));
    EXPECT_FALSE(enabled("Close Others"));
    EXPECT_FALSE(enabled("Close to the Right"));
    EXPECT_FALSE(enabled("Copy Path"));

    add("second");
    menu = strip_->build_context_menu(0);
    EXPECT_TRUE(enabled("Close Others"));
    EXPECT_TRUE(enabled("Close to the Right"));
    menu = strip_->build_context_menu(1);
    EXPECT_FALSE(enabled("Close to the Right"));
    EXPECT_TRUE(enabled("Copy Path"));
}

TEST_F(TabStripTest, AMenuActionFollowsItsTabWhenTheTabsChangeWhileTheMenuIsOpen) {
    add_many(4);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    const auto menu = strip_->build_context_menu(2);
    strip_->remove_tab(0);
    menu->actions().at(0)->trigger();
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).toInt(), 1);
}

TEST_F(TabStripTest, AMenuActionOfATabThatIsGoneDoesNothing) {
    add_many(3);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    QSignalSpy saved(strip_.get(), &TabStrip::close_saved_requested);
    const auto menu = strip_->build_context_menu(2);
    strip_->remove_tab(2);
    for (QAction* action : menu->actions()) {
        action->trigger();
    }
    EXPECT_EQ(closed.count() + saved.count(), 0);
}

TEST_F(TabStripTest, DraggingATabPastItsNeighborMovesIt) {
    add_many(3);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    const QPoint start = body(0);
    press(start);
    move(start + QPoint(5, 0), Qt::LeftButton);
    EXPECT_FALSE(strip_->is_dragging());
    EXPECT_EQ(moved.count(), 0);

    move(start + QPoint(100, 0), Qt::LeftButton);
    EXPECT_TRUE(strip_->is_dragging());
    EXPECT_EQ(moved.count(), 0);
    move(start + QPoint(140, 0), Qt::LeftButton);
    ASSERT_EQ(moved.count(), 1);
    EXPECT_EQ(moved.at(0).at(0).toInt(), 0);
    EXPECT_EQ(moved.at(0).at(1).toInt(), 1);
    EXPECT_EQ(titles(), (std::vector<QString>{"file1.txt", "file0.txt", "file2.txt"}));
    EXPECT_EQ(strip_->current_index(), 1);

    release(start + QPoint(140, 0));
    EXPECT_FALSE(strip_->is_dragging());
}

TEST_F(TabStripTest, DraggingFarMovesATabPastSeveralNeighborsOneStepAtATime) {
    add_many(4);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    const QPoint start = body(0);
    press(start);
    move(start + QPoint(12, 0), Qt::LeftButton);
    move(start + QPoint(390, 0), Qt::LeftButton);
    ASSERT_EQ(moved.count(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(moved.at(i).at(0).toInt(), i);
        EXPECT_EQ(moved.at(i).at(1).toInt(), i + 1);
    }
    EXPECT_EQ(titles(), (std::vector<QString>{"file1.txt", "file2.txt", "file3.txt", "file0.txt"}));
    EXPECT_EQ(strip_->current_index(), 3);
}

TEST_F(TabStripTest, DraggingBackMovesTheTabBack) {
    add_many(3);
    click(body(2));
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    const QPoint start = body(2);
    press(start);
    move(start - QPoint(12, 0), Qt::LeftButton);
    move(start - QPoint(140, 0), Qt::LeftButton);
    ASSERT_EQ(moved.count(), 1);
    EXPECT_EQ(moved.at(0).at(0).toInt(), 2);
    EXPECT_EQ(moved.at(0).at(1).toInt(), 1);
    EXPECT_EQ(titles(), (std::vector<QString>{"file0.txt", "file2.txt", "file1.txt"}));
    EXPECT_EQ(strip_->current_index(), 1);
}

TEST_F(TabStripTest, ADraggedTabIsDrawnWhereTheMouseHoldsIt) {
    add_many(3);
    const QPoint start = body(0);
    press(start);
    move(start + QPoint(12, 0), Qt::LeftButton);
    move(start + QPoint(40, 0), Qt::LeftButton);
    ASSERT_TRUE(strip_->is_dragging());
    const QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).left() + 40 + 5, 10)), QColor("#202020"));
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).left() + 3, 10)), QColor("#101010"));
    release(start + QPoint(40, 0));
    EXPECT_EQ(pixel(grab(), QPoint(strip_->tab_rect(0).left() + 3, 10)), QColor("#202020"));
}

TEST_F(TabStripTest, ADragToTheEndsOfTheStripMovesTheTabToTheFirstAndLastPlaces) {
    add_many(3);
    const QPoint start = body(0);
    press(start);
    move(start + QPoint(12, 0), Qt::LeftButton);
    move(QPoint(strip_->width() - 1, 10), Qt::LeftButton);
    EXPECT_EQ(strip_->current_index(), 2);
    move(QPoint(0, 10), Qt::LeftButton);
    EXPECT_EQ(strip_->current_index(), 0);
    release(QPoint(10, 10));
}

TEST_F(TabStripTest, MovingTheMouseWithoutAButtonDoesNotDrag) {
    add_many(3);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    move(body(0));
    move(body(2));
    EXPECT_FALSE(strip_->is_dragging());
    EXPECT_EQ(moved.count(), 0);
}

TEST_F(TabStripTest, ACloseButtonIsNotDraggable) {
    add_many(3);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    press(button(0));
    move(button(0) + QPoint(15, 0), Qt::LeftButton);
    move(button(0) + QPoint(200, 0), Qt::LeftButton);
    EXPECT_FALSE(strip_->is_dragging());
    EXPECT_EQ(moved.count(), 0);
    release(button(0) + QPoint(200, 0));
}

TEST_F(TabStripTest, ADragThatLeavesTheStripReportsThePointerAndMovesNoTab) {
    add_many(3);
    QSignalSpy out(strip_.get(), &TabStrip::drag_out_moved);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    press(body(1));
    move(body(1) + QPoint(0, 5), Qt::LeftButton);
    EXPECT_FALSE(strip_->is_dragging_out());
    move(QPoint(200, 120), Qt::LeftButton);
    EXPECT_TRUE(strip_->is_dragging());
    EXPECT_TRUE(strip_->is_dragging_out());
    ASSERT_EQ(out.count(), 1);
    EXPECT_EQ(out.at(0).at(0).toInt(), 1);
    EXPECT_EQ(out.at(0).at(1).toPoint(), strip_->mapToGlobal(QPoint(200, 120)));
    move(QPoint(900, 300), Qt::LeftButton);
    EXPECT_EQ(out.count(), 2);
    EXPECT_EQ(moved.count(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"file0.txt", "file1.txt", "file2.txt"}));
    release(QPoint(900, 300));
}

TEST_F(TabStripTest, ADragThatStartsDownwardsAlsoLeavesTheStrip) {
    add_many(2);
    QSignalSpy out(strip_.get(), &TabStrip::drag_out_moved);
    press(body(0));
    move(body(0) + QPoint(0, 80), Qt::LeftButton);
    EXPECT_TRUE(strip_->is_dragging_out());
    EXPECT_EQ(out.count(), 1);
    release(body(0) + QPoint(0, 80));
}

TEST_F(TabStripTest, ReleasingOutsideTheStripDropsTheTabThereAndSelectsNothing) {
    add_many(3);
    QSignalSpy dropped(strip_.get(), &TabStrip::drag_out_dropped);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    press(body(2));
    move(QPoint(100, 150), Qt::LeftButton);
    release(QPoint(100, 150));
    ASSERT_EQ(dropped.count(), 1);
    EXPECT_EQ(dropped.at(0).at(0).toInt(), 2);
    EXPECT_EQ(dropped.at(0).at(1).toPoint(), strip_->mapToGlobal(QPoint(100, 150)));
    EXPECT_FALSE(strip_->is_dragging_out());
    EXPECT_FALSE(strip_->is_dragging());
    EXPECT_EQ(closed.count(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"file0.txt", "file1.txt", "file2.txt"}));
}

TEST_F(TabStripTest, BringingTheTabBackOverTheStripEndsTheDragOutAndReordersAgain) {
    add_many(3);
    QSignalSpy cancelled(strip_.get(), &TabStrip::drag_out_cancelled);
    QSignalSpy dropped(strip_.get(), &TabStrip::drag_out_dropped);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    press(body(0));
    move(QPoint(100, 120), Qt::LeftButton);
    ASSERT_TRUE(strip_->is_dragging_out());
    move(body(0) + QPoint(140, 0), Qt::LeftButton);
    EXPECT_FALSE(strip_->is_dragging_out());
    EXPECT_EQ(cancelled.count(), 1);
    EXPECT_EQ(moved.count(), 1);
    release(body(0) + QPoint(140, 0));
    EXPECT_EQ(dropped.count(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"file1.txt", "file0.txt", "file2.txt"}));
}

TEST_F(TabStripTest, EscapeCancelsADragOutAndTheReleaseThatFollowsDropsNothing) {
    add_many(2);
    QSignalSpy cancelled(strip_.get(), &TabStrip::drag_out_cancelled);
    QSignalSpy dropped(strip_.get(), &TabStrip::drag_out_dropped);
    press(body(0));
    move(QPoint(100, 120), Qt::LeftButton);
    ASSERT_TRUE(strip_->is_dragging_out());
    QTest::keyClick(strip_.get(), Qt::Key_Escape);
    EXPECT_FALSE(strip_->is_dragging_out());
    EXPECT_FALSE(strip_->is_dragging());
    EXPECT_EQ(cancelled.count(), 1);
    move(QPoint(100, 130), Qt::LeftButton);
    release(QPoint(100, 130));
    EXPECT_EQ(dropped.count(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"file0.txt", "file1.txt"}));
}

TEST_F(TabStripTest, EscapeDoesNothingWhenNoTabIsDraggedOut) {
    add_many(2);
    QSignalSpy cancelled(strip_.get(), &TabStrip::drag_out_cancelled);
    QTest::keyClick(strip_.get(), Qt::Key_Escape);
    EXPECT_EQ(cancelled.count(), 0);
}

TEST_F(TabStripTest, ATabThatIsDraggedOutIsShownDimmedWhereItIs) {
    add_many(2);
    const QPoint inside = strip_->tab_rect(0).topLeft() + QPoint(5, 10);
    EXPECT_EQ(pixel(grab(), inside), QColor("#202020"));
    press(body(0));
    move(QPoint(100, 120), Qt::LeftButton);
    EXPECT_NE(pixel(grab(), inside), QColor("#202020"));
    EXPECT_EQ(strip_->tab_rect(0), QRect(0, 0, 120, 35));
    release(QPoint(100, 120));
    EXPECT_EQ(pixel(grab(), inside), QColor("#202020"));
}

TEST_F(TabStripTest, TheInsertionIndexIsTheNearestGapBetweenTabs) {
    add_many(3);
    EXPECT_EQ(strip_->insertion_index_at(0), 0);
    EXPECT_EQ(strip_->insertion_index_at(59), 0);
    EXPECT_EQ(strip_->insertion_index_at(61), 1);
    EXPECT_EQ(strip_->insertion_index_at(179), 1);
    EXPECT_EQ(strip_->insertion_index_at(181), 2);
    EXPECT_EQ(strip_->insertion_index_at(301), 3);
    EXPECT_EQ(strip_->insertion_index_at(590), 3);
}

TEST_F(TabStripTest, TheInsertionIndexCountsTheScroll) {
    add_many(12);
    strip_->set_current_index(11);
    const int scroll = strip_->scroll_offset();
    ASSERT_GT(scroll, 0);
    EXPECT_EQ(strip_->insertion_index_at(0), (scroll + 60) / 120);
}

TEST_F(TabStripTest, ADropIndicatorShowsALineWhereATabWouldGo) {
    colors_.set_colors({{"tab.activeBackground", QColor("#202020")},
                        {"tab.inactiveBackground", QColor("#303030")},
                        {"tab.dragAndDropBorder", QColor("#ff00ff")}});
    add_many(3);
    strip_->set_drop_indicator(1);
    QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(120, 10)), QColor("#ff00ff"));
    EXPECT_EQ(pixel(image, QPoint(119, 10)), QColor("#ff00ff"));
    EXPECT_NE(pixel(image, QPoint(130, 10)), QColor("#ff00ff"));

    strip_->set_drop_indicator(3);
    image = grab();
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(2).right(), 10)), QColor("#ff00ff"));
    EXPECT_NE(pixel(image, QPoint(120, 10)), QColor("#ff00ff"));

    strip_->set_drop_indicator(-1);
    image = grab();
    EXPECT_NE(pixel(image, QPoint(strip_->tab_rect(2).right(), 10)), QColor("#ff00ff"));
}

TEST_F(TabStripTest, SettingTheDropIndicatorToWhatItIsDoesNotRepaint) {
    add_many(2);
    strip_->set_drop_indicator(1);
    QApplication::processEvents();
    PaintCounter counter;
    strip_->installEventFilter(&counter);
    strip_->set_drop_indicator(1);
    strip_->set_drop_indicator(99);
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 1);
}

TEST_F(TabStripTest, TabsThatDoNotFitScrollAndTheSelectedTabStaysInView) {
    add_many(12);
    EXPECT_EQ(strip_->scroll_offset(), 0);
    strip_->set_current_index(11);
    const QRect last = strip_->tab_rect(11);
    EXPECT_GT(strip_->scroll_offset(), 0);
    EXPECT_GE(last.left(), 0);
    EXPECT_LE(last.right(), strip_->width() - 1);

    strip_->set_current_index(0);
    EXPECT_EQ(strip_->scroll_offset(), 0);
    EXPECT_EQ(strip_->tab_rect(0).left(), 0);
}

TEST_F(TabStripTest, TheWheelScrollsTheTabsAndStopsAtTheEnds) {
    add_many(12);
    const auto wheel = [&](int dx, int dy) {
        QWheelEvent event(QPointF(100, 10), QPointF(100, 10), QPoint(), QPoint(dx, dy),
                          Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QApplication::sendEvent(strip_.get(), &event);
        return event.isAccepted();
    };
    EXPECT_TRUE(wheel(0, -120));
    EXPECT_EQ(strip_->scroll_offset(), 120);
    EXPECT_TRUE(wheel(0, -240));
    EXPECT_EQ(strip_->scroll_offset(), 360);
    EXPECT_TRUE(wheel(0, 120));
    EXPECT_EQ(strip_->scroll_offset(), 240);
    EXPECT_TRUE(wheel(-120, 0));
    EXPECT_EQ(strip_->scroll_offset(), 360);
    wheel(0, 100000);
    EXPECT_EQ(strip_->scroll_offset(), 0);
    wheel(0, -100000);
    const int end = strip_->scroll_offset();
    EXPECT_EQ(strip_->tab_rect(11).right(), strip_->width() - 1) << end;
}

TEST_F(TabStripTest, TheWheelDoesNothingWhenEveryTabFits) {
    add_many(2);
    QWheelEvent event(QPointF(100, 10), QPointF(100, 10), QPoint(), QPoint(0, -120), Qt::NoButton,
                      {}, Qt::NoScrollPhase, false);
    QApplication::sendEvent(strip_.get(), &event);
    EXPECT_FALSE(event.isAccepted());
    EXPECT_EQ(strip_->scroll_offset(), 0);
}

TEST_F(TabStripTest, ClickingATabThatIsPartlyHiddenBringsItIntoView) {
    strip_->resize(500, TabStrip::height_pixels);
    add_many(12);
    strip_->set_current_index(0);
    const QRect partial = strip_->tab_rect(4);
    ASSERT_GT(partial.right(), strip_->width() - 1);
    ASSERT_LT(partial.left(), strip_->width() - 10);
    click(QPoint(partial.left() + 5, 10));
    EXPECT_EQ(strip_->current_index(), 4);
    EXPECT_LE(strip_->tab_rect(4).right(), strip_->width() - 1);
    EXPECT_GE(strip_->tab_rect(4).left(), 0);
}

TEST_F(TabStripTest, ClicksLandOnTheTabsThatAreShownWhileScrolled) {
    add_many(12);
    strip_->set_current_index(11);
    QSignalSpy spy(strip_.get(), &TabStrip::current_changed);
    click(body(10));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), 10);
}

TEST_F(TabStripTest, WideningTheStripKeepsTheSelectedTabVisibleAndDropsUnneededScroll) {
    add_many(12);
    strip_->set_current_index(11);
    ASSERT_GT(strip_->scroll_offset(), 0);
    strip_->resize(5000, TabStrip::height_pixels);
    EXPECT_EQ(strip_->scroll_offset(), 0);

    strip_->resize(300, TabStrip::height_pixels);
    EXPECT_GE(strip_->tab_rect(11).left(), 0);
    EXPECT_LE(strip_->tab_rect(11).right(), 299);
}

TEST_F(TabStripTest, RemovingTabsClampsTheScroll) {
    add_many(12);
    strip_->set_current_index(11);
    for (int i = 0; i < 10; ++i) {
        strip_->remove_tab(0);
    }
    EXPECT_EQ(strip_->scroll_offset(), 0);
}

TEST_F(TabStripTest, ATabShowsItsPathAsAToolTip) {
    add_many(2);
    const QRect area = strip_->tab_rect(1);
    QHelpEvent help(QEvent::ToolTip, area.center(), strip_->mapToGlobal(area.center()));
    QApplication::sendEvent(strip_.get(), &help);
    EXPECT_TRUE(QToolTip::isVisible());
    EXPECT_EQ(QToolTip::text(), "/path/file1.txt");

    QHelpEvent none(QEvent::ToolTip, QPoint(580, 10), strip_->mapToGlobal(QPoint(580, 10)));
    QApplication::sendEvent(strip_.get(), &none);
    EXPECT_TRUE(QTest::qWaitFor([] { return !QToolTip::isVisible(); }, 2000));
}

TEST_F(TabStripTest, TheStripRepaintsWithTheColorsOfANewTheme) {
    add_many(2);
    EXPECT_EQ(pixel(grab(), strip_->tab_rect(0).topLeft() + QPoint(5, 10)), QColor("#202020"));
    colors_.set_colors({{"tab.activeBackground", QColor("#112233")},
                        {"tab.inactiveBackground", QColor("#445566")}});
    const QImage image = grab();
    EXPECT_EQ(pixel(image, strip_->tab_rect(0).topLeft() + QPoint(5, 10)), QColor("#112233"));
    EXPECT_EQ(pixel(image, strip_->tab_rect(1).topLeft() + QPoint(5, 10)), QColor("#445566"));
}

TEST_F(TabStripTest, TheBordersBelowTheStripAndTheSelectedTabShowOnlyWhenTheThemeHasThem) {
    add_many(2);
    const int bottom = TabStrip::height_pixels - 1;
    QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(550, bottom)), QColor("#101010"));
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).left() + 5, bottom)), QColor("#202020"));

    colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#101010")},
                        {"editorGroupHeader.tabsBorder", QColor("#00ff00")},
                        {"tab.activeBorder", QColor("#ff0000")},
                        {"tab.activeBackground", QColor("#202020")},
                        {"tab.inactiveBackground", QColor("#303030")}});
    image = grab();
    EXPECT_EQ(pixel(image, QPoint(550, bottom)), QColor("#00ff00"));
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(1).left() + 5, bottom)), QColor("#303030"));
    EXPECT_EQ(pixel(image, QPoint(strip_->tab_rect(0).left() + 5, bottom)), QColor("#ff0000"));
}

TEST_F(TabStripTest, AStripThatDoesNotHaveTheFocusUsesTheUnfocusedColors) {
    colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#101010")},
                        {"tab.activeBackground", QColor("#202020")},
                        {"tab.inactiveBackground", QColor("#303030")},
                        {"tab.activeBorderTop", QColor("#3994bc")},
                        {"tab.unfocusedActiveBackground", QColor("#252525")},
                        {"tab.unfocusedInactiveBackground", QColor("#353535")},
                        {"tab.activeForeground", QColor("#ffffff")},
                        {"tab.unfocusedActiveForeground", QColor("#ff0000")}});
    add_many(2);
    EXPECT_TRUE(strip_->focused());
    strip_->set_focused(false);
    EXPECT_FALSE(strip_->focused());
    const QImage image = grab();
    const QRect active = strip_->tab_rect(0);
    EXPECT_EQ(pixel(image, active.topLeft() + QPoint(5, 10)), QColor("#252525"));
    EXPECT_EQ(pixel(image, strip_->tab_rect(1).topLeft() + QPoint(5, 10)), QColor("#353535"));
    EXPECT_EQ(pixel(image, active.topLeft() + QPoint(5, 0)), QColor("#252525"));

    strip_->set_focused(true);
    const QImage focused = grab();
    EXPECT_EQ(pixel(focused, active.topLeft() + QPoint(5, 10)), QColor("#202020"));
    EXPECT_EQ(pixel(focused, active.topLeft() + QPoint(5, 0)), QColor("#3994bc"));
}

TEST_F(TabStripTest, TheLineOnTopOfTheSelectedTabOfAnUnfocusedStripShowsWhenTheThemeHasOne) {
    colors_.set_colors({{"tab.activeBackground", QColor("#202020")},
                        {"tab.activeBorderTop", QColor("#3994bc")},
                        {"tab.unfocusedActiveBorderTop", QColor("#112233")}});
    add_many(1);
    strip_->set_focused(false);
    EXPECT_EQ(pixel(grab(), strip_->tab_rect(0).topLeft() + QPoint(5, 0)), QColor("#112233"));
}

TEST_F(TabStripTest, TheTitlesOfAnUnfocusedStripUseTheUnfocusedForegrounds) {
    colors_.set_colors({{"tab.activeBackground", QColor("#000000")},
                        {"tab.inactiveBackground", QColor("#000000")},
                        {"tab.activeForeground", QColor("#ffffff")},
                        {"tab.unfocusedActiveForeground", QColor("#00ff00")}});
    add("WWWWWW");
    strip_->set_focused(false);
    const QImage image = grab();
    const QRect area = strip_->tab_rect(0);
    int green = 0;
    int white = 0;
    for (int y = 2; y < 30; ++y) {
        for (int x = area.left() + 10; x < area.left() + 60; ++x) {
            const QColor c(image.pixel(x, y));
            green += c.green() > 200 && c.red() < 60;
            white += c.red() > 200 && c.green() > 200 && c.blue() > 200;
        }
    }
    EXPECT_GT(green, 0);
    EXPECT_EQ(white, 0);
}

TEST_F(TabStripTest, SettingTheFocusToWhatItIsDoesNotRepaint) {
    add_many(2);
    QApplication::processEvents();
    PaintCounter counter;
    strip_->installEventFilter(&counter);
    strip_->set_focused(true);
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 0);
    strip_->set_focused(false);
    QApplication::processEvents();
    EXPECT_EQ(counter.count, 1);
}

TEST_F(TabStripTest, RendersAStripForReview) {
    colors_.set_colors({{"editorGroupHeader.tabsBackground", QColor("#191A1B")},
                        {"editorGroupHeader.tabsBorder", QColor("#2A2B2C")},
                        {"tab.activeBackground", QColor("#121314")},
                        {"tab.inactiveBackground", QColor("#191A1B")},
                        {"tab.hoverBackground", QColor("#121314")},
                        {"tab.activeForeground", QColor("#bfbfbf")},
                        {"tab.inactiveForeground", QColor("#8C8C8C")},
                        {"tab.activeBorderTop", QColor("#3994BC")},
                        {"tab.activeBorder", QColor("#121314")},
                        {"tab.border", QColor("#2A2B2C")},
                        {"toolbar.hoverBackground", QColor("#5A5D5E50")}});
    strip_->resize(720, TabStrip::height_pixels);
    add("main_window.cpp", true);
    add("README.md");
    add("tab_strip.hpp", true);
    add("CMakeLists.txt");
    add("a_considerably_longer_file_name.cpp");
    strip_->set_current_index(1);
    move(button(3));
    const QImage image = grab();
    ASSERT_FALSE(image.isNull());
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/tab_strip.png");
    }
}

TEST_F(TabStripTest, SingleModeShowsOnlyTheSelectedTabAcrossTheWholeStrip) {
    add_many(4);
    strip_->set_current_index(2);
    strip_->set_single(true);
    EXPECT_TRUE(strip_->single());
    for (int i = 0; i < 4; ++i) {
        if (i == 2) {
            EXPECT_EQ(strip_->tab_rect(i), QRect(0, 0, 600, TabStrip::height_pixels));
        } else {
            EXPECT_TRUE(strip_->tab_rect(i).isEmpty()) << i;
        }
    }
    EXPECT_EQ(strip_->count(), 4);
    EXPECT_EQ(strip_->current_index(), 2);

    const QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(300, 10)), QColor("#202020"));
    EXPECT_EQ(pixel(image, QPoint(580, 10)), QColor("#202020"));
}

TEST_F(TabStripTest, SingleModeTakesTheWidthOfTheStripAsItResizes) {
    add("a.txt");
    strip_->set_single(true);
    strip_->resize(900, TabStrip::height_pixels);
    EXPECT_EQ(strip_->tab_rect(0).width(), 900);
    strip_->resize(300, TabStrip::height_pixels);
    EXPECT_EQ(strip_->tab_rect(0).width(), 300);
}

TEST_F(TabStripTest, ATitleLongerThanTheStripKeepsItsWidthInSingleModeAndDoesNotScroll) {
    add(QString(120, QLatin1Char('x')));
    strip_->set_single(true);
    EXPECT_GT(strip_->tab_rect(0).width(), 600);
    EXPECT_EQ(strip_->scroll_offset(), 0);
    QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(-120, 0), Qt::NoButton, {},
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(strip_.get(), &wheel);
    EXPECT_EQ(strip_->scroll_offset(), 0);
}

TEST_F(TabStripTest, SingleModeFollowsTheSelection) {
    add_many(3);
    strip_->set_single(true);
    EXPECT_FALSE(strip_->tab_rect(0).isEmpty());
    strip_->set_current_index(1);
    EXPECT_TRUE(strip_->tab_rect(0).isEmpty());
    EXPECT_FALSE(strip_->tab_rect(1).isEmpty());
    strip_->remove_tab(1);
    EXPECT_EQ(strip_->current_index(), 1);
    EXPECT_FALSE(strip_->tab_rect(1).isEmpty());
    EXPECT_TRUE(strip_->tab_rect(0).isEmpty());
}

TEST_F(TabStripTest, SingleModeCloseButtonAndMiddleClickCloseTheShownTab) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->set_single(true);
    QSignalSpy closed(strip_.get(), &TabStrip::close_requested);
    click(button(1));
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.takeFirst().at(0).toInt(), 1);
    click(body(1), Qt::MiddleButton);
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.takeFirst().at(0).toInt(), 1);
}

TEST_F(TabStripTest, SingleModeHasNothingToSelectWithTheMouseAndNothingToReorder) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->set_single(true);
    QSignalSpy selected(strip_.get(), &TabStrip::current_changed);
    QSignalSpy moved(strip_.get(), &TabStrip::tab_moved);
    press(QPoint(10, 10));
    move(QPoint(300, 10), Qt::LeftButton);
    move(QPoint(590, 10), Qt::LeftButton);
    release(QPoint(590, 10));
    EXPECT_EQ(selected.count(), 0);
    EXPECT_EQ(moved.count(), 0);
    EXPECT_EQ(titles(), (std::vector<QString>{"file0.txt", "file1.txt", "file2.txt"}));
    EXPECT_EQ(strip_->current_index(), 1);
}

TEST_F(TabStripTest, SingleModeShowsATooltipOnlyForTheShownTab) {
    add_many(2);
    strip_->set_single(true);
    EXPECT_EQ(strip_->hovered_tab(), -1);
    move(QPoint(100, 10));
    EXPECT_EQ(strip_->hovered_tab(), 0);
}

TEST_F(TabStripTest, SingleModeListsOnlyTheShownTabForAssistiveTechnology) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->set_single(true);
    const auto items = strip_->accessible_items();
    ASSERT_EQ(items.size(), 1U);
    EXPECT_EQ(items[0].name, "file1.txt");
    EXPECT_TRUE(items[0].state.selected);
    EXPECT_EQ(items[0].rect, QRect(0, 0, 600, TabStrip::height_pixels));
}

TEST_F(TabStripTest, LeavingSingleModeShowsEveryTabAgain) {
    add_many(3);
    strip_->set_current_index(2);
    strip_->set_single(true);
    strip_->set_single(false);
    EXPECT_FALSE(strip_->single());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(strip_->tab_rect(i).left(), i * TabStrip::minimum_tab_width) << i;
        EXPECT_EQ(strip_->tab_rect(i).width(), TabStrip::minimum_tab_width) << i;
    }
}

TEST_F(TabStripTest, ADropOverASingleTabGoesBeforeItOrAfterTheLastTab) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->set_single(true);
    EXPECT_EQ(strip_->insertion_index_at(10), 1);
    EXPECT_EQ(strip_->insertion_index_at(590), 3);
}

TEST_F(TabStripTest, ADropIndicatorOverASingleTabIsAtItsLeftEdgeBeforeItAndAtTheRightEdgeAfter) {
    add_many(3);
    strip_->set_current_index(1);
    strip_->set_single(true);
    const QColor line = colors_.color("tab.dragAndDropBorder");
    ASSERT_NE(line, QColor("#202020"));

    strip_->set_drop_indicator(1);
    QImage image = grab();
    EXPECT_EQ(pixel(image, QPoint(0, 10)), line);
    EXPECT_NE(pixel(image, QPoint(599, 10)), line);

    strip_->set_drop_indicator(3);
    image = grab();
    EXPECT_EQ(pixel(image, QPoint(599, 10)), line);
    EXPECT_NE(pixel(image, QPoint(0, 10)), line);
}

// --- icons ---

namespace {

// The default file icon, red on a dark theme and blue on a light one.
const SetiIcon test_icon{{0xE023, 0xFFFF0000}, {0xE023, 0xFF0000FF}};
const QString long_title(40, QLatin1Char('H'));

class TabStripIconTest : public TabStripTest {
protected:
    void SetUp() override {
        TabStripTest::SetUp();
        theme(anvil::core::ThemeKind::dark);
        strip_->resize(900, TabStrip::height_pixels);
        strip_->add_tab({.title = long_title, .icon = test_icon});
        strip_->add_tab({.title = long_title});
        QApplication::processEvents();
    }
    void theme(anvil::core::ThemeKind kind) {
        WorkbenchColors::Colors colors;
        for (const char* key :
             {"tab.activeBackground", "tab.inactiveBackground", "editorGroupHeader.tabsBackground",
              "tab.border", "tab.activeBorderTop"}) {
            colors[key] = QColor(0, 0, 0);
        }
        for (const char* key : {"tab.activeForeground", "tab.inactiveForeground"}) {
            colors[key] = QColor(0, 255, 0);
        }
        colors_.set_theme(colors, nullptr, kind);
    }
    // The first column of the tab, from its left, with a pixel mostly of one channel, or -1.
    int first_column(int tab, int channel) const {
        const QImage image = strip_->grab().toImage();
        const qreal ratio = image.devicePixelRatio();
        const QRect area = strip_->tab_rect(tab);
        for (int x = static_cast<int>(area.left() * ratio);
             x < static_cast<int>(area.right() * ratio) && x < image.width(); ++x) {
            for (int y = 0; y < image.height(); ++y) {
                const QColor color = image.pixelColor(x, y);
                const int channels[] = {color.red(), color.green(), color.blue()};
                const int others = channels[0] + channels[1] + channels[2] - channels[channel];
                if (channels[channel] > 150 && others < 80) {
                    return static_cast<int>(x / ratio) - area.left();
                }
            }
        }
        return -1;
    }
    static constexpr int red = 0;
    static constexpr int green = 1;
    static constexpr int blue = 2;
};

} // namespace

TEST_F(TabStripIconTest, TheIconIsBeforeTheTitleWhichMakesRoomForIt) {
    const int icon = first_column(0, red);
    ASSERT_GE(icon, TabStrip::title_padding - 4);
    EXPECT_LT(icon, TabStrip::title_padding + TabStrip::icon_width);
    EXPECT_EQ(first_column(1, red), -1);
    // The titles are the same, so that only the column of the icon tells them apart.
    EXPECT_EQ(first_column(0, green) - first_column(1, green), 22);
    EXPECT_EQ(strip_->tab_rect(0).width() - strip_->tab_rect(1).width(), 22);
}

TEST_F(TabStripIconTest, TheIconIsThatOfTheKindOfTheTheme) {
    ASSERT_GE(first_column(0, red), 0);
    EXPECT_EQ(first_column(0, blue), -1);
    theme(anvil::core::ThemeKind::light);
    QApplication::processEvents();
    EXPECT_GE(first_column(0, blue), 0);
    EXPECT_EQ(first_column(0, red), -1);
}

TEST_F(TabStripIconTest, ATabGivenAnotherIconIsPaintedAgainAndLaysOut) {
    PaintCounter paints;
    strip_->installEventFilter(&paints);
    strip_->set_tab(1, {.title = long_title});
    QApplication::processEvents();
    EXPECT_EQ(paints.count, 0);

    const int width = strip_->tab_rect(1).width();
    strip_->set_tab(1, {.title = long_title, .icon = test_icon});
    QApplication::processEvents();
    EXPECT_GT(paints.count, 0);
    EXPECT_EQ(strip_->tab_rect(1).width(), width + 22);

    paints.count = 0;
    SetiIcon other = test_icon;
    other.dark.color = 0xFF00FFFF;
    strip_->set_tab(1, {.title = long_title, .icon = other});
    QApplication::processEvents();
    EXPECT_GT(paints.count, 0);
    strip_->removeEventFilter(&paints);
}

// --- descriptions and the empty part of the strip ---

TEST_F(TabStripIconTest, ADescriptionWidensItsTabAndIsDrawnDimmerAfterTheTitle) {
    const int width = strip_->tab_rect(1).width();
    strip_->set_tab(1, {.title = long_title, .description = long_title});
    QApplication::processEvents();
    EXPECT_GT(strip_->tab_rect(1).width(), width + 100);

    // The title is green, and the description a dimmer green on the black of the tab.
    const QImage image = strip_->grab().toImage();
    const QRect tab = strip_->tab_rect(1);
    const auto brightest = [&](int from, int to) {
        int best = 0;
        for (int x = from; x < to; ++x) {
            for (int y = 0; y < image.height(); ++y) {
                best = std::max(best, image.pixelColor(x, y).green());
            }
        }
        return best;
    };
    const int title_end = tab.left() + TabStrip::title_padding +
                          QFontMetrics(strip_->font()).horizontalAdvance(long_title);
    EXPECT_GT(brightest(tab.left(), title_end), 220);
    const int description =
        brightest(title_end + TabStrip::description_gap, tab.right() - TabStrip::action_width);
    EXPECT_GT(description, 100);
    EXPECT_LT(description, 200);

    PaintCounter paints;
    strip_->installEventFilter(&paints);
    strip_->set_tab(1, {.title = long_title, .description = "other"});
    QApplication::processEvents();
    EXPECT_GT(paints.count, 0);
    EXPECT_LT(strip_->tab_rect(1).width(), width + 100);
    strip_->removeEventFilter(&paints);
}

TEST_F(TabStripIconTest, ADoubleClickWhereNoTabIsAsksForANewFile) {
    int asked = 0;
    QObject::connect(strip_.get(), &TabStrip::new_file_requested, [&] { ++asked; });
    strip_->resize(1600, TabStrip::height_pixels);
    QApplication::processEvents();
    const QPoint empty(strip_->tab_rect(1).right() + 40, 10);
    ASSERT_TRUE(strip_->rect().contains(empty));
    QTest::mouseClick(strip_.get(), Qt::LeftButton, Qt::NoModifier, empty);
    EXPECT_EQ(asked, 0);
    QTest::mouseDClick(strip_.get(), Qt::LeftButton, Qt::NoModifier, empty);
    EXPECT_EQ(asked, 1);
    QTest::mouseDClick(strip_.get(), Qt::RightButton, Qt::NoModifier, empty);
    QTest::mouseDClick(strip_.get(), Qt::LeftButton, Qt::NoModifier, strip_->tab_rect(1).center());
    EXPECT_EQ(asked, 1);
    // The one tab of the single mode takes the whole strip.
    strip_->set_single(true);
    QApplication::processEvents();
    QTest::mouseDClick(strip_.get(), Qt::LeftButton, Qt::NoModifier, empty);
    EXPECT_EQ(asked, 1);
}
