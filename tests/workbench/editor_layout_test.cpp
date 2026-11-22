#include "anvil/workbench/editor_layout.hpp"

#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

using namespace anvil::workbench;
using anvil::core::Uri;
using nlohmann::json;

namespace {

Uri doc(const std::string& name) {
    return Uri{"file", "", "/work/" + name, "", ""};
}

GroupId id(std::uint32_t value) {
    return GroupId{value};
}

std::vector<std::string> names(const EditorGroup& group) {
    std::vector<std::string> result;
    for (const Uri& uri : group.tabs) {
        result.push_back(uri.path.substr(6));
    }
    return result;
}

using Names = std::vector<std::string>;

class Recorder : public EditorLayout::Observer {
public:
    void structure_changed() override {
        events.push_back("structure");
    }
    void ratios_changed() override {
        events.push_back("ratios");
    }
    void tabs_changed(GroupId group) override {
        events.push_back("tabs " + std::to_string(static_cast<unsigned>(group)));
    }
    void active_tab_changed(GroupId group) override {
        events.push_back("active " + std::to_string(static_cast<unsigned>(group)));
    }
    void focus_changed(GroupId group) override {
        events.push_back("focus " + std::to_string(static_cast<unsigned>(group)));
    }
    std::vector<std::string> events;
};

class EditorLayoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        layout_.add_observer(&recorder_);
    }

    void open(GroupId group, std::initializer_list<const char*> files) {
        for (const char* file : files) {
            ASSERT_TRUE(layout_.open(group, doc(file)).has_value());
        }
        recorder_.events.clear();
    }

    Names tabs(GroupId group) const {
        return names(*layout_.group(group));
    }

    void expect_consistent() {
        EXPECT_EQ(layout_.find_inconsistency(), std::nullopt);
    }

    EditorLayout layout_;
    Recorder recorder_;
};

} // namespace

TEST_F(EditorLayoutTest, StartsWithOneEmptyFocusedGroup) {
    ASSERT_EQ(layout_.group_ids().size(), 1U);
    const GroupId first = layout_.group_ids()[0];
    EXPECT_EQ(layout_.focused(), first);
    EXPECT_TRUE(layout_.root().is_group());
    EXPECT_EQ(layout_.root().group, first);
    EXPECT_TRUE(layout_.group(first)->tabs.empty());
    EXPECT_EQ(layout_.group(first)->active, std::nullopt);
    expect_consistent();
}

TEST_F(EditorLayoutTest, AnUnknownGroupIsNotFound) {
    EXPECT_EQ(layout_.group(id(99)), nullptr);
    EXPECT_EQ(layout_.open(id(99), doc("a")), std::nullopt);
    EXPECT_FALSE(layout_.activate(id(99), 0));
    EXPECT_FALSE(layout_.close_tab(id(99), 0));
    EXPECT_FALSE(layout_.focus(id(99)));
    EXPECT_FALSE(layout_.close_group(id(99)));
    EXPECT_EQ(layout_.split(id(99), Direction::right), std::nullopt);
    EXPECT_FALSE(layout_.move_tab(id(99), 0, id(1), 0));
}

TEST_F(EditorLayoutTest, OpeningADocumentAddsTheTabAndActivatesIt) {
    const GroupId g = layout_.focused();
    EXPECT_EQ(layout_.open(g, doc("a")), 0U);
    EXPECT_EQ(tabs(g), (Names{"a"}));
    EXPECT_EQ(layout_.group(g)->active, 0U);
    expect_consistent();
}

TEST_F(EditorLayoutTest, ANewTabGoesRightOfTheActiveTab) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    ASSERT_TRUE(layout_.activate(g, 0));
    EXPECT_EQ(layout_.open(g, doc("d")), 1U);
    EXPECT_EQ(tabs(g), (Names{"a", "d", "b", "c"}));
    EXPECT_EQ(layout_.group(g)->active, 1U);
    expect_consistent();
}

TEST_F(EditorLayoutTest, OpeningATabThatIsOpenActivatesItAndAddsNothing) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    EXPECT_EQ(layout_.open(g, doc("a")), 0U);
    EXPECT_EQ(tabs(g), (Names{"a", "b", "c"}));
    EXPECT_EQ(layout_.group(g)->active, 0U);
}

TEST_F(EditorLayoutTest, ADocumentCanBeOpenInSeveralGroupsButOnceInEach) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"b", "b", "c"});
    EXPECT_EQ(tabs(first), (Names{"a", "b"}));
    EXPECT_EQ(tabs(second), (Names{"b", "c"}));
    EXPECT_EQ(layout_.locations(doc("b")), (std::vector<TabLocation>{{first, 1}, {second, 0}}));
    EXPECT_EQ(layout_.locations(doc("c")), (std::vector<TabLocation>{{second, 1}}));
    EXPECT_TRUE(layout_.locations(doc("zzz")).empty());
    expect_consistent();
}

TEST_F(EditorLayoutTest, ShowOpensAnUnopenedDocumentInTheFocusedGroup) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    layout_.focus(first);
    const TabLocation location = layout_.show(doc("a"));
    EXPECT_EQ(location, (TabLocation{first, 0}));
    EXPECT_EQ(layout_.focused(), first);
    EXPECT_EQ(tabs(first), (Names{"a"}));
    EXPECT_TRUE(tabs(second).empty());
}

TEST_F(EditorLayoutTest, ShowFocusesTheTabOfADocumentThatIsOpenInAnotherGroup) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"c"});
    ASSERT_EQ(layout_.focused(), second);
    ASSERT_TRUE(layout_.activate(first, 1));

    const TabLocation location = layout_.show(doc("a"));
    EXPECT_EQ(location, (TabLocation{first, 0}));
    EXPECT_EQ(layout_.focused(), first);
    EXPECT_EQ(layout_.group(first)->active, 0U);
    EXPECT_EQ(tabs(second), (Names{"c"}));
    EXPECT_EQ(layout_.locations(doc("a")).size(), 1U);
}

TEST_F(EditorLayoutTest, ShowPrefersTheFocusedGroupWhenSeveralHaveTheDocument) {
    const GroupId first = layout_.focused();
    open(first, {"a"});
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    open(second, {"b", "a"});
    layout_.focus(second);
    ASSERT_TRUE(layout_.activate(second, 0));
    EXPECT_EQ(layout_.show(doc("a")), (TabLocation{second, 0}));
    EXPECT_EQ(layout_.focused(), second);
    EXPECT_EQ(layout_.group(second)->active, 0U);

    layout_.focus(first);
    EXPECT_EQ(layout_.show(doc("a")), (TabLocation{first, 0}));
}

TEST_F(EditorLayoutTest, ActivatingATabChecksTheIndex) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b"});
    EXPECT_TRUE(layout_.activate(g, 0));
    EXPECT_EQ(layout_.group(g)->active, 0U);
    EXPECT_FALSE(layout_.activate(g, 2));
    EXPECT_EQ(layout_.group(g)->active, 0U);
}

TEST_F(EditorLayoutTest, ClosingTheActiveTabActivatesItsRightNeighbor) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    ASSERT_TRUE(layout_.activate(g, 1));
    EXPECT_TRUE(layout_.close_tab(g, 1));
    EXPECT_EQ(tabs(g), (Names{"a", "c"}));
    EXPECT_EQ(layout_.group(g)->active, 1U);
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "c");
}

TEST_F(EditorLayoutTest, ClosingTheActiveLastTabActivatesItsLeftNeighbor) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    EXPECT_TRUE(layout_.close_tab(g, 2));
    EXPECT_EQ(layout_.group(g)->active, 1U);
    EXPECT_EQ(tabs(g), (Names{"a", "b"}));
}

TEST_F(EditorLayoutTest, ClosingATabThatIsNotActiveKeepsTheActiveTab) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    ASSERT_TRUE(layout_.activate(g, 1));
    EXPECT_TRUE(layout_.close_tab(g, 0));
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "b");
    EXPECT_TRUE(layout_.close_tab(g, 1));
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "b");
}

TEST_F(EditorLayoutTest, ClosingTheLastTabOfTheOnlyGroupLeavesItEmpty) {
    const GroupId g = layout_.focused();
    open(g, {"a"});
    EXPECT_TRUE(layout_.close_tab(g, 0));
    ASSERT_EQ(layout_.group_ids().size(), 1U);
    EXPECT_TRUE(layout_.group(g)->tabs.empty());
    EXPECT_EQ(layout_.group(g)->active, std::nullopt);
    EXPECT_FALSE(layout_.close_tab(g, 0));
    expect_consistent();
}

TEST_F(EditorLayoutTest, ClosingTheLastTabOfAGroupClosesTheGroupWhenThereAreOthers) {
    const GroupId first = layout_.focused();
    open(first, {"a"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"b"});
    EXPECT_TRUE(layout_.close_tab(second, 0));
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{first}));
    EXPECT_EQ(layout_.focused(), first);
    EXPECT_TRUE(layout_.root().is_group());
    expect_consistent();
}

TEST(EditorLayoutOptionsTest, EmptyGroupsStayWhenTheOptionIsOff) {
    EditorLayout layout({.close_empty_groups = false});
    const GroupId first = layout.focused();
    layout.open(first, doc("a"));
    const GroupId second = *layout.split(first, Direction::right);
    layout.open(second, doc("b"));
    EXPECT_TRUE(layout.close_tab(second, 0));
    EXPECT_EQ(layout.group_ids().size(), 2U);
    EXPECT_TRUE(layout.group(second)->tabs.empty());
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);

    EXPECT_TRUE(layout.move_tab(first, 0, second, 0));
    EXPECT_EQ(layout.group_ids().size(), 2U);
}

TEST_F(EditorLayoutTest, ASplitIsNotClosedWhileItIsEmpty) {
    const GroupId first = layout_.focused();
    open(first, {"a"});
    const GroupId second = *layout_.split(first, Direction::right);
    EXPECT_EQ(layout_.group_ids().size(), 2U);
    EXPECT_TRUE(layout_.group(second)->tabs.empty());
}

TEST_F(EditorLayoutTest, ATabMovesWithinItsGroup) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c", "d"});
    ASSERT_TRUE(layout_.activate(g, 1));
    EXPECT_TRUE(layout_.move_tab(g, 0, g, 2));
    EXPECT_EQ(tabs(g), (Names{"b", "c", "a", "d"}));
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "b");
    EXPECT_TRUE(layout_.move_tab(g, 3, g, 0));
    EXPECT_EQ(tabs(g), (Names{"d", "b", "c", "a"}));
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "b");
    EXPECT_TRUE(layout_.move_tab(g, 1, g, 100));
    EXPECT_EQ(tabs(g), (Names{"d", "c", "a", "b"}));
    EXPECT_EQ(names(*layout_.group(g))[*layout_.group(g)->active], "b");
    expect_consistent();
}

TEST_F(EditorLayoutTest, MovingATabToItsOwnPlaceChangesNothing) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    EXPECT_TRUE(layout_.move_tab(g, 1, g, 1));
    EXPECT_EQ(tabs(g), (Names{"a", "b", "c"}));
    EXPECT_TRUE(recorder_.events.empty());
}

TEST_F(EditorLayoutTest, ATabMovesToAnotherGroupAndIsActiveThereWithTheFocus) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b", "c"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"x", "y"});
    layout_.focus(first);
    ASSERT_TRUE(layout_.activate(first, 1));

    EXPECT_TRUE(layout_.move_tab(first, 1, second, 1));
    EXPECT_EQ(tabs(first), (Names{"a", "c"}));
    EXPECT_EQ(names(*layout_.group(first))[*layout_.group(first)->active], "c");
    EXPECT_EQ(tabs(second), (Names{"x", "b", "y"}));
    EXPECT_EQ(layout_.group(second)->active, 1U);
    EXPECT_EQ(layout_.focused(), second);
    expect_consistent();
}

TEST_F(EditorLayoutTest, ATabMovedPastTheEndOfAnotherGroupGoesToItsEnd) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"x"});
    EXPECT_TRUE(layout_.move_tab(first, 0, second, 50));
    EXPECT_EQ(tabs(second), (Names{"x", "a"}));
}

TEST_F(EditorLayoutTest, MovingATabToAGroupThatHasTheDocumentActivatesTheExistingTab) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"b", "x"});
    ASSERT_TRUE(layout_.activate(second, 1));
    EXPECT_TRUE(layout_.move_tab(first, 1, second, 1));
    EXPECT_EQ(tabs(first), (Names{"a"}));
    EXPECT_EQ(tabs(second), (Names{"b", "x"}));
    EXPECT_EQ(layout_.group(second)->active, 0U);
    EXPECT_EQ(layout_.focused(), second);
}

TEST_F(EditorLayoutTest, MovingTheLastTabOutOfAGroupClosesTheGroup) {
    const GroupId first = layout_.focused();
    open(first, {"a"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"x"});
    EXPECT_TRUE(layout_.move_tab(second, 0, first, 0));
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{first}));
    EXPECT_EQ(tabs(first), (Names{"x", "a"}));
    EXPECT_EQ(layout_.focused(), first);
    expect_consistent();
}

TEST_F(EditorLayoutTest, MovingATabChecksItsArguments) {
    const GroupId g = layout_.focused();
    open(g, {"a"});
    EXPECT_FALSE(layout_.move_tab(g, 1, g, 0));
    EXPECT_FALSE(layout_.move_tab(g, 0, id(77), 0));
    EXPECT_FALSE(layout_.move_tab(id(77), 0, g, 0));
}

TEST_F(EditorLayoutTest, ReplacingADocumentKeepsItsTabInPlaceAndActive) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    ASSERT_TRUE(layout_.activate(g, 1));
    EXPECT_EQ(layout_.replace_document(doc("b"), doc("renamed")), 1U);
    EXPECT_EQ(tabs(g), (Names{"a", "renamed", "c"}));
    EXPECT_EQ(layout_.group(g)->active, 1U);
    EXPECT_TRUE(layout_.locations(doc("b")).empty());
    expect_consistent();
}

TEST_F(EditorLayoutTest, ReplacingADocumentChangesItInEveryGroupThatHasIt) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right);
    open(second, {"b", "x"});
    recorder_.events.clear();
    EXPECT_EQ(layout_.replace_document(doc("b"), doc("c")), 2U);
    EXPECT_EQ(tabs(first), (Names{"a", "c"}));
    EXPECT_EQ(tabs(second), (Names{"c", "x"}));
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"tabs 1", "active 1", "tabs 2"}));
}

TEST_F(EditorLayoutTest, ReplacingADocumentWithOneThatTheGroupHasClosesTheOldTab) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    ASSERT_TRUE(layout_.activate(g, 0));
    EXPECT_EQ(layout_.replace_document(doc("a"), doc("c")), 1U);
    EXPECT_EQ(tabs(g), (Names{"b", "c"}));
    EXPECT_EQ(layout_.group(g)->active, 0U);
    ASSERT_TRUE(layout_.activate(g, 1));
    EXPECT_EQ(layout_.replace_document(doc("b"), doc("c")), 1U);
    EXPECT_EQ(tabs(g), (Names{"c"}));
    EXPECT_EQ(layout_.group(g)->active, 0U);
    expect_consistent();
}

TEST_F(EditorLayoutTest, ReplacingADocumentThatIsNotOpenOrWithItselfDoesNothing) {
    const GroupId g = layout_.focused();
    open(g, {"a"});
    EXPECT_EQ(layout_.replace_document(doc("zzz"), doc("b")), 0U);
    EXPECT_EQ(layout_.replace_document(doc("a"), doc("a")), 0U);
    EXPECT_EQ(tabs(g), (Names{"a"}));
    EXPECT_TRUE(recorder_.events.empty());
}

TEST_F(EditorLayoutTest, SplittingRightPutsANewGroupAfterTheOldOneAndFocusesIt) {
    const GroupId first = layout_.focused();
    const auto second = layout_.split(first, Direction::right);
    ASSERT_TRUE(second);
    const LayoutNode& root = layout_.root();
    ASSERT_FALSE(root.is_group());
    EXPECT_EQ(root.orientation, Orientation::horizontal);
    ASSERT_EQ(root.children.size(), 2U);
    EXPECT_EQ(root.children[0].group, first);
    EXPECT_EQ(root.children[1].group, second);
    EXPECT_EQ(root.ratios, (std::vector<double>{0.5, 0.5}));
    EXPECT_EQ(layout_.focused(), second);
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{first, *second}));
    expect_consistent();
}

TEST_F(EditorLayoutTest, SplittingDownStacksTheGroups) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::down);
    EXPECT_EQ(layout_.root().orientation, Orientation::vertical);
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{first, second}));
}

TEST_F(EditorLayoutTest, SplittingLeftOrUpPutsTheNewGroupFirst) {
    const GroupId first = layout_.focused();
    const GroupId left = *layout_.split(first, Direction::left);
    EXPECT_EQ(layout_.root().orientation, Orientation::horizontal);
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{left, first}));
    const GroupId up = *layout_.split(first, Direction::up);
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{left, up, first}));
    expect_consistent();
}

TEST_F(EditorLayoutTest, SplittingAlongTheAxisOfTheParentAddsASiblingThatTakesHalfTheShare) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    layout_.set_ratios({}, {1, 3});
    const GroupId third = *layout_.split(second, Direction::right);
    const LayoutNode& root = layout_.root();
    ASSERT_EQ(root.children.size(), 3U);
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{first, second, third}));
    EXPECT_NEAR(root.ratios[0], 0.25, 1e-12);
    EXPECT_NEAR(root.ratios[1], 0.375, 1e-12);
    EXPECT_NEAR(root.ratios[2], 0.375, 1e-12);
    expect_consistent();
}

TEST_F(EditorLayoutTest, SplittingAcrossTheAxisOfTheParentNestsASplitter) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    const GroupId third = *layout_.split(second, Direction::down);
    const LayoutNode& root = layout_.root();
    ASSERT_EQ(root.children.size(), 2U);
    EXPECT_EQ(root.children[0].group, first);
    const LayoutNode& nested = root.children[1];
    ASSERT_FALSE(nested.is_group());
    EXPECT_EQ(nested.orientation, Orientation::vertical);
    EXPECT_EQ(nested.children[0].group, second);
    EXPECT_EQ(nested.children[1].group, third);
    EXPECT_EQ(root.ratios, (std::vector<double>{0.5, 0.5}));
    EXPECT_EQ(nested.ratios, (std::vector<double>{0.5, 0.5}));
    expect_consistent();
}

TEST_F(EditorLayoutTest, ASplitCanShowTheActiveDocumentOfTheOriginalGroup) {
    const GroupId first = layout_.focused();
    open(first, {"a", "b"});
    const GroupId second = *layout_.split(first, Direction::right, SplitContent::copy_active);
    EXPECT_EQ(tabs(second), (Names{"b"}));
    EXPECT_EQ(layout_.group(second)->active, 0U);
    EXPECT_EQ(tabs(first), (Names{"a", "b"}));
    const GroupId third = *layout_.split(second, Direction::down, SplitContent::empty);
    EXPECT_TRUE(tabs(third).empty());
}

TEST_F(EditorLayoutTest, CopyingTheActiveDocumentOfAnEmptyGroupGivesAnEmptyGroup) {
    const GroupId second =
        *layout_.split(layout_.focused(), Direction::right, SplitContent::copy_active);
    EXPECT_TRUE(tabs(second).empty());
}

TEST_F(EditorLayoutTest, GroupIdsAreNotReused) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    ASSERT_TRUE(layout_.close_group(second));
    const GroupId third = *layout_.split(first, Direction::right);
    EXPECT_NE(third, second);
    EXPECT_NE(third, first);
}

TEST_F(EditorLayoutTest, ClosingAGroupGivesItsSpaceToThePreviousSibling) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    const GroupId third = *layout_.split(second, Direction::right);
    layout_.set_ratios({}, {1, 2, 5});
    ASSERT_TRUE(layout_.close_group(third));
    EXPECT_EQ(layout_.root().children.size(), 2U);
    EXPECT_NEAR(layout_.root().ratios[0], 0.125, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[1], 0.875, 1e-12);
    expect_consistent();
}

TEST_F(EditorLayoutTest, ClosingTheFirstGroupGivesItsSpaceToTheNextOne) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    const GroupId third = *layout_.split(second, Direction::right);
    layout_.set_ratios({}, {2, 3, 5});
    ASSERT_TRUE(layout_.close_group(first));
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{second, third}));
    EXPECT_NEAR(layout_.root().ratios[0], 0.5, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[1], 0.5, 1e-12);
}

TEST_F(EditorLayoutTest, ASplitterLeftWithOneChildIsReplacedByIt) {
    const GroupId first = layout_.focused();
    const GroupId second = *layout_.split(first, Direction::right);
    ASSERT_TRUE(layout_.close_group(second));
    EXPECT_TRUE(layout_.root().is_group());
    EXPECT_EQ(layout_.root().group, first);
}

TEST_F(EditorLayoutTest, AnInnerSplitterOfTheOuterOrientationIsMergedIntoIt) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::down);
    const GroupId d = *layout_.split(c, Direction::right);
    ASSERT_TRUE(layout_.close_group(b));
    ASSERT_EQ(layout_.root().children.size(), 3U);
    EXPECT_NEAR(layout_.root().ratios[0], 0.5, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[1], 0.25, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[2], 0.25, 1e-12);
    ASSERT_TRUE(layout_.close_group(c));
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{a, d}));
    EXPECT_EQ(layout_.root().orientation, Orientation::horizontal);
    EXPECT_TRUE(layout_.root().children[0].is_group());
    EXPECT_TRUE(layout_.root().children[1].is_group());
    expect_consistent();
}

TEST_F(EditorLayoutTest, AGroupClosedAtTheTopLeavesTheSplitterBelowItWithItsOwnRatios) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::down);
    const GroupId d = *layout_.split(c, Direction::down);
    ASSERT_TRUE(layout_.set_ratios({1}, {1, 1, 2}));
    ASSERT_TRUE(layout_.set_ratios({}, {1, 3}));
    ASSERT_TRUE(layout_.close_group(a));
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{b, c, d}));
    ASSERT_EQ(layout_.root().children.size(), 3U);
    EXPECT_EQ(layout_.root().orientation, Orientation::vertical);
    EXPECT_NEAR(layout_.root().ratios[0], 0.25, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[1], 0.25, 1e-12);
    EXPECT_NEAR(layout_.root().ratios[2], 0.5, 1e-12);
    expect_consistent();
}

TEST_F(EditorLayoutTest, TheOnlyGroupCannotBeClosed) {
    EXPECT_FALSE(layout_.close_group(layout_.focused()));
    EXPECT_EQ(layout_.group_ids().size(), 1U);
}

TEST_F(EditorLayoutTest, ClosingTheFocusedGroupFocusesThePreviousOneOrTheNextWhenItWasFirst) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::right);
    ASSERT_TRUE(layout_.focus(b));
    ASSERT_TRUE(layout_.close_group(b));
    EXPECT_EQ(layout_.focused(), a);
    ASSERT_TRUE(layout_.focus(a));
    ASSERT_TRUE(layout_.close_group(a));
    EXPECT_EQ(layout_.focused(), c);
}

TEST_F(EditorLayoutTest, ClosingAnotherGroupKeepsTheFocus) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::right);
    ASSERT_TRUE(layout_.focus(a));
    ASSERT_TRUE(layout_.close_group(c));
    EXPECT_EQ(layout_.focused(), a);
    (void)b;
}

TEST_F(EditorLayoutTest, ClosingAGroupDropsItsTabs) {
    const GroupId a = layout_.focused();
    open(a, {"x"});
    const GroupId b = *layout_.split(a, Direction::right);
    open(b, {"y"});
    ASSERT_TRUE(layout_.close_group(b));
    EXPECT_TRUE(layout_.locations(doc("y")).empty());
    EXPECT_EQ(layout_.group(b), nullptr);
}

TEST_F(EditorLayoutTest, FocusMovesToTheNextAndPreviousGroupAndWraps) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(a, Direction::down);
    // The order shown: a, c above... a and c are stacked in the first column, then b.
    EXPECT_EQ(layout_.group_ids(), (std::vector<GroupId>{a, c, b}));
    ASSERT_TRUE(layout_.focus(a));
    layout_.focus_next();
    EXPECT_EQ(layout_.focused(), c);
    layout_.focus_next();
    EXPECT_EQ(layout_.focused(), b);
    layout_.focus_next();
    EXPECT_EQ(layout_.focused(), a);
    layout_.focus_previous();
    EXPECT_EQ(layout_.focused(), b);
    layout_.focus_previous();
    EXPECT_EQ(layout_.focused(), c);
}

TEST_F(EditorLayoutTest, FocusNextWithOneGroupStays) {
    const GroupId a = layout_.focused();
    layout_.focus_next();
    layout_.focus_previous();
    EXPECT_EQ(layout_.focused(), a);
    EXPECT_TRUE(recorder_.events.empty());
}

TEST_F(EditorLayoutTest, RatiosAreSetByPathAndScaledToOne) {
    const GroupId a = layout_.focused();
    const GroupId b = *layout_.split(a, Direction::right);
    const GroupId c = *layout_.split(b, Direction::down);
    (void)c;
    EXPECT_TRUE(layout_.set_ratios({}, {1, 3}));
    EXPECT_EQ(layout_.root().ratios, (std::vector<double>{0.25, 0.75}));
    EXPECT_TRUE(layout_.set_ratios({1}, {30, 10}));
    EXPECT_EQ(layout_.root().children[1].ratios, (std::vector<double>{0.75, 0.25}));
    expect_consistent();
}

TEST_F(EditorLayoutTest, RatiosThatAreWrongAreRefused) {
    const GroupId a = layout_.focused();
    *layout_.split(a, Direction::right);
    EXPECT_FALSE(layout_.set_ratios({}, {1}));
    EXPECT_FALSE(layout_.set_ratios({}, {1, 2, 3}));
    EXPECT_FALSE(layout_.set_ratios({}, {1, 0}));
    EXPECT_FALSE(layout_.set_ratios({}, {1, -1}));
    EXPECT_FALSE(layout_.set_ratios({}, {1, std::nan("")}));
    EXPECT_FALSE(layout_.set_ratios({0}, {1}));
    EXPECT_FALSE(layout_.set_ratios({5}, {1, 1}));
    EXPECT_EQ(layout_.root().ratios, (std::vector<double>{0.5, 0.5}));
}

TEST_F(EditorLayoutTest, OpeningATabTellsAboutTheTabsAndTheActiveTab) {
    const GroupId g = layout_.focused();
    layout_.open(g, doc("a"));
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"tabs 1", "active 1"}));
    recorder_.events.clear();
    layout_.open(g, doc("a"));
    EXPECT_TRUE(recorder_.events.empty());
    layout_.open(g, doc("b"));
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"tabs 1", "active 1"}));
}

TEST_F(EditorLayoutTest, ActivatingATabTellsOnlyAboutTheActiveTab) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b"});
    layout_.activate(g, 0);
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"active 1"}));
}

TEST_F(EditorLayoutTest, ClosingATabThatIsNotActiveTellsOnlyAboutTheTabs) {
    const GroupId g = layout_.focused();
    open(g, {"a", "b", "c"});
    layout_.close_tab(g, 0);
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"tabs 1"}));
}

TEST_F(EditorLayoutTest, SplittingTellsAboutTheStructureAndTheFocus) {
    layout_.split(layout_.focused(), Direction::right);
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"structure", "focus 2"}));
}

TEST_F(EditorLayoutTest, ChangingRatiosTellsOnlyAboutTheRatios) {
    layout_.split(layout_.focused(), Direction::right);
    recorder_.events.clear();
    layout_.set_ratios({}, {1, 3});
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"ratios"}));
    recorder_.events.clear();
    layout_.set_ratios({}, {1, 3});
    EXPECT_TRUE(recorder_.events.empty());
}

TEST_F(EditorLayoutTest, ClosingAGroupTellsAboutTheStructureAndTheFocusButNotAboutItsTabs) {
    const GroupId a = layout_.focused();
    open(a, {"a"});
    const GroupId b = *layout_.split(a, Direction::right);
    open(b, {"b"});
    layout_.close_group(b);
    EXPECT_EQ(recorder_.events, (std::vector<std::string>{"structure", "focus 1"}));
}

TEST_F(EditorLayoutTest, MovingATabBetweenGroupsTellsAboutBothGroupsAndTheFocus) {
    const GroupId a = layout_.focused();
    open(a, {"a", "b"});
    const GroupId b = *layout_.split(a, Direction::right);
    open(b, {"x"});
    layout_.focus(a);
    recorder_.events.clear();
    layout_.move_tab(a, 0, b, 0);
    EXPECT_EQ(recorder_.events,
              (std::vector<std::string>{"tabs 1", "tabs 2", "active 2", "focus 2"}));
}

TEST_F(EditorLayoutTest, FocusingTheFocusedGroupTellsNothing) {
    layout_.focus(layout_.focused());
    EXPECT_TRUE(recorder_.events.empty());
}

TEST_F(EditorLayoutTest, AnObserverIsToldAfterTheOperationIsDone) {
    struct Checker : EditorLayout::Observer {
        explicit Checker(EditorLayout& l) : layout(l) {}
        void structure_changed() override {
            consistent = !layout.find_inconsistency().has_value();
            groups = layout.group_ids().size();
        }
        EditorLayout& layout;
        bool consistent = false;
        std::size_t groups = 0;
    } checker(layout_);
    layout_.add_observer(&checker);
    layout_.split(layout_.focused(), Direction::right);
    EXPECT_TRUE(checker.consistent);
    EXPECT_EQ(checker.groups, 2U);
}

TEST_F(EditorLayoutTest, ARemovedObserverIsNotToldAndOneCanRemoveItselfWhileBeingTold) {
    struct Quitter : EditorLayout::Observer {
        explicit Quitter(EditorLayout& l) : layout(l) {}
        void focus_changed(GroupId) override {
            ++calls;
            layout.remove_observer(this);
        }
        EditorLayout& layout;
        int calls = 0;
    } quitter(layout_);
    layout_.add_observer(&quitter);
    layout_.remove_observer(&recorder_);
    layout_.split(layout_.focused(), Direction::right);
    layout_.focus_previous();
    layout_.focus_next();
    EXPECT_EQ(quitter.calls, 1);
    EXPECT_TRUE(recorder_.events.empty());
}

namespace {

// A layout with several groups, splitters of both orientations, uneven ratios and tabs.
EditorLayout make_complex_layout() {
    EditorLayout layout;
    const GroupId a = layout.focused();
    layout.open(a, doc("a1"));
    layout.open(a, doc("a2"));
    const GroupId b = *layout.split(a, Direction::right);
    layout.open(b, doc("b1"));
    const GroupId c = *layout.split(b, Direction::down, SplitContent::copy_active);
    layout.open(c, doc("c1"));
    layout.open(c, Uri{"untitled", "", "Untitled-1", "", ""});
    layout.open(c, Uri{"vscode-remote", "host", "/p ath/é", "q=1", "frag"});
    const GroupId d = *layout.split(a, Direction::up);
    (void)d;
    layout.set_ratios({}, {1, 2});
    layout.set_ratios({1}, {0.3, 0.9});
    layout.activate(a, 0);
    layout.focus(c);
    return layout;
}

} // namespace

TEST(EditorLayoutJsonTest, ALayoutRoundTrips) {
    const EditorLayout layout = make_complex_layout();
    const json saved = layout.to_json();
    const auto restored = EditorLayout::from_json(saved);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored->to_json(), saved);
    EXPECT_EQ(restored->root(), layout.root());
    EXPECT_EQ(restored->group_ids(), layout.group_ids());
    EXPECT_EQ(restored->focused(), layout.focused());
    for (const GroupId group : layout.group_ids()) {
        EXPECT_EQ(*restored->group(group), *layout.group(group));
    }
    EXPECT_EQ(restored->find_inconsistency(), std::nullopt);
}

TEST(EditorLayoutJsonTest, ALayoutSurvivesBeingWrittenAsText) {
    const EditorLayout layout = make_complex_layout();
    const auto restored = EditorLayout::from_json(json::parse(layout.to_json().dump()));
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored->root(), layout.root());
}

TEST(EditorLayoutJsonTest, TheDefaultLayoutRoundTrips) {
    const EditorLayout layout;
    const auto restored = EditorLayout::from_json(layout.to_json());
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->to_json(), layout.to_json());
}

TEST(EditorLayoutJsonTest, ARestoredLayoutDoesNotReuseGroupIds) {
    EditorLayout layout;
    const GroupId second = *layout.split(layout.focused(), Direction::right);
    layout.close_group(second);
    auto restored = EditorLayout::from_json(layout.to_json());
    ASSERT_TRUE(restored);
    const GroupId third = *restored->split(restored->focused(), Direction::right);
    EXPECT_NE(third, second);
}

TEST(EditorLayoutJsonTest, ARestoredLayoutWorks) {
    auto restored = EditorLayout::from_json(make_complex_layout().to_json());
    ASSERT_TRUE(restored);
    const GroupId target = restored->group_ids().front();
    EXPECT_TRUE(restored->open(target, doc("new")).has_value());
    EXPECT_TRUE(restored->close_group(restored->group_ids().back()));
    EXPECT_EQ(restored->find_inconsistency(), std::nullopt);
}

namespace {

json valid_two_groups() {
    EditorLayout layout;
    layout.open(layout.focused(), doc("a"));
    layout.split(layout.focused(), Direction::right);
    return layout.to_json();
}

} // namespace

TEST(EditorLayoutJsonTest, ALayoutThatBreaksTheRulesIsRefused) {
    const auto refused = [](const std::function<void(json&)>& damage) {
        json saved = valid_two_groups();
        damage(saved);
        return !EditorLayout::from_json(saved).has_value();
    };
    EXPECT_FALSE(refused([](json&) {}));
    EXPECT_TRUE(refused([](json& j) { j["version"] = 2; }));
    EXPECT_TRUE(refused([](json& j) { j.erase("version"); }));
    EXPECT_TRUE(refused([](json& j) { j["focused"] = 99; }));
    EXPECT_TRUE(refused([](json& j) { j["next_group_id"] = 1; }));
    EXPECT_TRUE(refused([](json& j) { j.erase("root"); }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["ratios"] = json::array({0.5, 0.2}); }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["ratios"] = json::array({1.5, -0.5}); }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["ratios"] = json::array({1.0}); }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["orientation"] = "diagonal"; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][1]["group"] = 1; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["active"] = 3; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["active"] = nullptr; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][1]["active"] = 0; }));
    EXPECT_TRUE(refused([](json& j) {
        j["root"]["children"][0]["tabs"].push_back(j["root"]["children"][0]["tabs"][0]);
    }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["tabs"] = "a"; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["tabs"][0] = 5; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["tabs"][0]["scheme"] = 5; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["tabs"][0].erase("scheme"); }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"][0]["group"] = "one"; }));
    EXPECT_TRUE(refused([](json& j) { j["root"]["children"] = "none"; }));
    EXPECT_TRUE(refused([](json& j) { j["root"] = 3; }));
}

TEST(EditorLayoutJsonTest, ASplitterWithOneChildOrOfItsParentsOrientationIsRefused) {
    json single = valid_two_groups();
    single["root"]["children"].erase(1);
    single["root"]["ratios"] = json::array({1.0});
    EXPECT_FALSE(EditorLayout::from_json(single));

    json nested = valid_two_groups();
    json inner = nested["root"];
    nested["root"]["children"][1] = inner;
    nested["root"]["children"][1]["children"][0]["group"] = 10;
    nested["root"]["children"][1]["children"][1]["group"] = 11;
    nested["next_group_id"] = 12;
    EXPECT_FALSE(EditorLayout::from_json(nested));
}

TEST(EditorLayoutJsonTest, ANotJsonObjectIsRefused) {
    EXPECT_FALSE(EditorLayout::from_json(json::array()));
    EXPECT_FALSE(EditorLayout::from_json(json(nullptr)));
    const auto refused = EditorLayout::from_json(json::object());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, anvil::ErrorCode::invalid_argument);
}
