#include "anvil/ui/editor_area.hpp"

#include <QApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QTest>
#include <gtest/gtest.h>
#include <map>
#include <memory>

using namespace anvil::ui;
using anvil::core::Uri;
using anvil::workbench::Direction;
using anvil::workbench::EditorLayout;
using anvil::workbench::GroupId;
using anvil::workbench::SplitContent;

namespace {

Uri doc(const std::string& name) {
    return Uri{"file", "", "/work/" + name, "", ""};
}

class EditorAreaTest : public ::testing::Test {
protected:
    void SetUp() override {
        area_ = std::make_unique<EditorArea>(
            layout_, colors_,
            [this](const Uri& uri) {
                ++info_calls_;
                return TabStrip::Tab{.title = QString::fromStdString(uri.path.substr(6)),
                                     .tooltip = {},
                                     .dirty = false,
                                     .has_path = true};
            },
            [this](const Uri& uri) -> std::shared_ptr<anvil::core::Document> {
                auto found = documents_.find(uri);
                if (found == documents_.end()) {
                    found = documents_
                                .emplace(uri, std::make_shared<anvil::core::Document>(
                                                  *anvil::core::Text::from_utf8(uri.path)))
                                .first;
                }
                return found->second;
            });
        area_->resize(800, 600);
        area_->show();
        QApplication::processEvents();
    }

    std::vector<QString> titles(GroupId id) {
        std::vector<QString> result;
        TabStrip* strip = area_->group(id)->tabs();
        for (int i = 0; i < strip->count(); ++i) {
            result.push_back(strip->tab(i).title);
        }
        return result;
    }

    WorkbenchColors colors_{QApplication::palette()};
    EditorLayout layout_;
    std::map<Uri, std::shared_ptr<anvil::core::Document>> documents_;
    int info_calls_ = 0;
    std::unique_ptr<EditorArea> area_;
};

} // namespace

TEST_F(EditorAreaTest, StartsWithOneEmptyGroupAndNoSplitter) {
    ASSERT_EQ(area_->groups().size(), 1U);
    EXPECT_EQ(area_->root_splitter(), nullptr);
    EXPECT_EQ(area_->focused_group(), area_->groups()[0]);
    EXPECT_TRUE(area_->groups()[0]->is_empty());
    EXPECT_FALSE(area_->groups()[0]->view()->isVisible());
    EXPECT_TRUE(area_->groups()[0]->tabs()->isVisible());
}

TEST_F(EditorAreaTest, TheTabsOfAGroupFollowTheLayout) {
    const GroupId g = layout_.focused();
    layout_.open(g, doc("a"));
    layout_.open(g, doc("b"));
    layout_.open(g, doc("c"));
    EXPECT_EQ(titles(g), (std::vector<QString>{"a", "b", "c"}));
    EXPECT_EQ(area_->group(g)->tabs()->current_index(), 2);

    layout_.activate(g, 0);
    EXPECT_EQ(area_->group(g)->tabs()->current_index(), 0);
    layout_.move_tab(g, 0, g, 2);
    EXPECT_EQ(titles(g), (std::vector<QString>{"b", "c", "a"}));
    layout_.close_tab(g, 1);
    EXPECT_EQ(titles(g), (std::vector<QString>{"b", "a"}));
    layout_.replace_document(doc("b"), doc("z"));
    EXPECT_EQ(titles(g), (std::vector<QString>{"z", "a"}));
}

TEST_F(EditorAreaTest, TheEditorOfAGroupShowsItsActiveDocument) {
    const GroupId g = layout_.focused();
    layout_.open(g, doc("a"));
    EXPECT_FALSE(area_->group(g)->is_empty());
    EXPECT_TRUE(area_->group(g)->view()->isVisible());
    EXPECT_EQ(area_->group(g)->view()->document()->text().to_string(), "/work/a");
    layout_.open(g, doc("b"));
    EXPECT_EQ(area_->group(g)->view()->document()->text().to_string(), "/work/b");
    layout_.activate(g, 0);
    EXPECT_EQ(area_->group(g)->view()->document()->text().to_string(), "/work/a");
    layout_.close_tab(g, 0);
    layout_.close_tab(g, 0);
    EXPECT_TRUE(area_->group(g)->is_empty());
    EXPECT_EQ(area_->group(g)->view()->document(), nullptr);
    EXPECT_FALSE(area_->group(g)->view()->isVisible());
}

TEST_F(EditorAreaTest, ASplitBuildsASplitterWithAGroupWidgetOnEachSide) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    ASSERT_EQ(area_->groups().size(), 2U);
    QSplitter* splitter = area_->root_splitter();
    ASSERT_NE(splitter, nullptr);
    EXPECT_EQ(splitter->orientation(), Qt::Horizontal);
    ASSERT_EQ(splitter->count(), 2);
    EXPECT_EQ(splitter->widget(0), area_->group(first));
    EXPECT_EQ(splitter->widget(1), area_->group(second));
    EXPECT_EQ(titles(second), (std::vector<QString>{"a"}));
    EXPECT_NEAR(area_->group(first)->width(), area_->group(second)->width(), 3);
}

TEST_F(EditorAreaTest, TheGroupWidgetsAreTheSameOnesAfterASplitSoNothingLosesItsState) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    EditorGroupWidget* before = area_->group(first);
    layout_.split(first, Direction::right);
    layout_.split(first, Direction::down);
    EXPECT_EQ(area_->group(first), before);
    EXPECT_EQ(titles(first), (std::vector<QString>{"a"}));
}

TEST_F(EditorAreaTest, ClosingAGroupRemovesItsWidgetAndTheSplitter) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    QPointer<EditorGroupWidget> closed = area_->group(second);
    layout_.close_group(second);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(closed.isNull());
    EXPECT_EQ(area_->groups().size(), 1U);
    EXPECT_EQ(area_->root_splitter(), nullptr);
    EXPECT_TRUE(area_->group(first)->isVisible());
    EXPECT_EQ(area_->group(second), nullptr);
}

TEST_F(EditorAreaTest, NestedSplitsBuildNestedSplitters) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::down);
    QSplitter* root = area_->root_splitter();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->count(), 2);
    auto* nested = qobject_cast<QSplitter*>(root->widget(1));
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->orientation(), Qt::Vertical);
    EXPECT_EQ(nested->widget(0), area_->group(b));
    EXPECT_EQ(nested->widget(1), area_->group(c));
}

TEST_F(EditorAreaTest, TheRatiosOfTheLayoutSetTheSizesOfTheSplitter) {
    const GroupId first = layout_.focused();
    layout_.split(first, Direction::right);
    layout_.set_ratios({}, {1, 3});
    QApplication::processEvents();
    const auto groups = area_->groups();
    EXPECT_NEAR(static_cast<double>(groups[0]->width()) / groups[1]->width(), 1.0 / 3.0, 0.05);
}

TEST_F(EditorAreaTest, TheFocusedGroupHasTheKeyboardFocusAndTheFocusedStrip) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    EXPECT_TRUE(area_->group(second)->tabs()->focused());
    EXPECT_FALSE(area_->group(first)->tabs()->focused());
    EXPECT_TRUE(area_->group(second)->view()->hasFocus());
    layout_.focus(first);
    EXPECT_TRUE(area_->group(first)->tabs()->focused());
    EXPECT_FALSE(area_->group(second)->tabs()->focused());
    EXPECT_TRUE(area_->group(first)->view()->hasFocus());
}

TEST_F(EditorAreaTest, AnEmptyFocusedGroupTakesTheKeyboardFocusItself) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    const GroupId second = *layout_.split(first, Direction::right);
    EXPECT_TRUE(area_->group(second)->hasFocus());
    EXPECT_FALSE(area_->group(first)->view()->hasFocus());
}

TEST_F(EditorAreaTest, FocusMovingIntoAGroupIsReportedAndTheLayoutIsLeftToTheWindow) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    QSignalSpy spy(area_.get(), &EditorArea::focus_requested);
    area_->group(first)->view()->setFocus();
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<GroupId>(), first);
    EXPECT_EQ(layout_.focused(), second);
}

TEST_F(EditorAreaTest, TheFocusMovingInsideTheFocusedGroupIsNotReported) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    QSignalSpy spy(area_.get(), &EditorArea::focus_requested);
    area_->group(first)->find_bar()->open();
    area_->group(first)->view()->setFocus();
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(EditorAreaTest, AGroupKeepsItsFindBarFocusWhenTheLayoutFocusMovesToIt) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    layout_.split(first, Direction::right, SplitContent::copy_active);
    area_->group(first)->find_bar()->open();
    QWidget* field = QApplication::focusWidget();
    ASSERT_NE(field, nullptr);
    ASSERT_NE(field, area_->group(first)->view());
    ASSERT_TRUE(area_->group(first)->isAncestorOf(field));
    layout_.focus(first);
    EXPECT_EQ(QApplication::focusWidget(), field);
}

TEST_F(EditorAreaTest, ADocumentRenamedInAGroupKeepsItsEditorWhereItWas) {
    const GroupId g = layout_.focused();
    auto document = std::make_shared<anvil::core::Document>(
        *anvil::core::Text::from_utf8(std::string(5000, '\n')));
    documents_[doc("a")] = document;
    layout_.open(g, doc("a"));
    EditorView* view = area_->group(g)->view();
    view->verticalScrollBar()->setValue(40);
    ASSERT_EQ(view->verticalScrollBar()->value(), 40);

    documents_[doc("z")] = document;
    layout_.replace_document(doc("a"), doc("z"));
    EXPECT_EQ(titles(g), (std::vector<QString>{"z"}));
    EXPECT_EQ(view->document(), document);
    EXPECT_EQ(view->verticalScrollBar()->value(), 40);
}

TEST_F(EditorAreaTest, TheActionsOfAGroupsStripAreReportedWithItsId) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    layout_.open(first, doc("b"));
    const GroupId second = *layout_.split(first, Direction::right);
    layout_.open(second, doc("c"));
    QSignalSpy selected(area_.get(), &EditorArea::tab_selected);
    QSignalSpy closed(area_.get(), &EditorArea::close_requested);

    TabStrip* strip = area_->group(second)->tabs();
    const QPoint button = strip->close_button_rect(0).center();
    QMouseEvent press(QEvent::MouseButtonPress, button, strip->mapToGlobal(button), Qt::LeftButton,
                      Qt::LeftButton, {});
    QApplication::sendEvent(strip, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, button, strip->mapToGlobal(button),
                        Qt::LeftButton, Qt::NoButton, {});
    QApplication::sendEvent(strip, &release);
    ASSERT_EQ(closed.count(), 1);
    EXPECT_EQ(closed.at(0).at(0).value<GroupId>(), second);
    EXPECT_EQ(closed.at(0).at(1).toInt(), 0);

    TabStrip* left = area_->group(first)->tabs();
    const QPoint tab = left->tab_rect(0).center() - QPoint(10, 0);
    QMouseEvent press_tab(QEvent::MouseButtonPress, tab, left->mapToGlobal(tab), Qt::LeftButton,
                          Qt::LeftButton, {});
    QApplication::sendEvent(left, &press_tab);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).value<GroupId>(), first);
    EXPECT_EQ(selected.at(0).at(1).toInt(), 0);
}

TEST_F(EditorAreaTest, TypingInAGroupIsReportedWithItsId) {
    const GroupId first = layout_.focused();
    layout_.open(first, doc("a"));
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    QSignalSpy spy(area_.get(), &EditorArea::text_changed);
    QKeyEvent key(QEvent::KeyPress, 'x', Qt::NoModifier, "x");
    QApplication::sendEvent(area_->group(second)->view(), &key);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<GroupId>(), second);
}

TEST_F(EditorAreaTest, TheSettingsReachEveryGroupIncludingLaterOnes) {
    anvil::core::EditorSettings settings;
    settings.font_size = 22;
    area_->apply_settings(settings);
    const GroupId second = *layout_.split(layout_.focused(), Direction::right);
    EXPECT_EQ(area_->group(second)->view()->line_height(),
              area_->groups()[0]->view()->line_height());
    EXPECT_GT(area_->group(second)->view()->line_height(), 22);
}

TEST_F(EditorAreaTest, RefreshingATabAsksForItsDescriptionAndChangesNothingWhenItIsTheSame) {
    const GroupId g = layout_.focused();
    layout_.open(g, doc("a"));
    area_->refresh_tab(g, 0);
    const int before = info_calls_;
    area_->refresh_tab(g, 0);
    area_->refresh_tab(g, 5);
    area_->refresh_tab(GroupId{99}, 0);
    EXPECT_EQ(info_calls_, before + 1);
    EXPECT_EQ(titles(g), (std::vector<QString>{"a"}));
}

TEST_F(EditorAreaTest, ADocumentThatIsEmptyForAGroupOnlyThroughTheLookupIsShownAsEmpty) {
    const GroupId g = layout_.focused();
    layout_.open(g, doc("a"));
    ASSERT_FALSE(area_->group(g)->is_empty());
    documents_.clear();
    layout_.activate(g, 0);
    EXPECT_FALSE(area_->group(g)->is_empty());
}
