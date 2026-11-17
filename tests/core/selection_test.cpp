#include "anvil/core/selection.hpp"

#include <gtest/gtest.h>

using namespace anvil::core;

TEST(Selection, DescribesItsExtentAndDirection) {
    const Selection forward{2, 5};
    EXPECT_EQ(forward.start(), 2u);
    EXPECT_EQ(forward.end(), 5u);
    EXPECT_FALSE(forward.reversed());
    EXPECT_FALSE(forward.empty());

    const Selection backward{5, 2};
    EXPECT_EQ(backward.start(), 2u);
    EXPECT_EQ(backward.end(), 5u);
    EXPECT_TRUE(backward.reversed());

    EXPECT_TRUE(Selection::caret(3).empty());
}

TEST(SelectionSet, DefaultsToACaretAtTheStart) {
    const SelectionSet set;
    ASSERT_EQ(set.all().size(), 1u);
    EXPECT_EQ(set.primary(), Selection::caret(0));
}

TEST(SelectionSet, EmptyInputBecomesTheDefaultCaret) {
    EXPECT_EQ(SelectionSet::from({}), SelectionSet{});
}

TEST(SelectionSet, SortsSelectionsAndTracksThePrimary) {
    const auto set = SelectionSet::from({{20, 25}, {0, 3}, {10, 12}}, 0);
    ASSERT_EQ(set.all().size(), 3u);
    EXPECT_EQ(set.all()[0], (Selection{0, 3}));
    EXPECT_EQ(set.all()[1], (Selection{10, 12}));
    EXPECT_EQ(set.all()[2], (Selection{20, 25}));
    EXPECT_EQ(set.primary_index(), 2u);
    EXPECT_EQ(set.primary(), (Selection{20, 25}));
}

TEST(SelectionSet, MergesOverlappingSelections) {
    const auto set = SelectionSet::from({{0, 5}, {3, 9}, {20, 22}});
    ASSERT_EQ(set.all().size(), 2u);
    EXPECT_EQ(set.all()[0], (Selection{0, 9}));
    EXPECT_EQ(set.all()[1], (Selection{20, 22}));
}

TEST(SelectionSet, MergesNestedSelections) {
    const auto set = SelectionSet::from({{0, 10}, {2, 4}});
    ASSERT_EQ(set.all().size(), 1u);
    EXPECT_EQ(set.all()[0], (Selection{0, 10}));
}

TEST(SelectionSet, KeepsTouchingNonEmptySelectionsApart) {
    const auto set = SelectionSet::from({{0, 3}, {3, 6}});
    EXPECT_EQ(set.all().size(), 2u);
}

TEST(SelectionSet, MergesACaretThatTouchesASelection) {
    EXPECT_EQ(SelectionSet::from({{0, 3}, {3, 3}}).all(), (std::vector<Selection>{{0, 3}}));
    EXPECT_EQ(SelectionSet::from({{3, 3}, {3, 6}}).all(), (std::vector<Selection>{{3, 6}}));
    EXPECT_EQ(SelectionSet::from({{4, 4}, {4, 4}}).all(), (std::vector<Selection>{{4, 4}}));
}

TEST(SelectionSet, MergedSelectionKeepsThePrimarysDirectionAndIndex) {
    const auto set = SelectionSet::from({{0, 5}, {9, 3}, {30, 31}}, 1);
    ASSERT_EQ(set.all().size(), 2u);
    EXPECT_EQ(set.all()[0], (Selection{9, 0}));
    EXPECT_EQ(set.primary_index(), 0u);
}

TEST(SelectionSet, ClampsAnOutOfRangePrimaryIndex) {
    const auto set = SelectionSet::from({{1, 1}, {5, 5}}, 99);
    EXPECT_EQ(set.primary(), Selection::caret(5));
}
