// Tests of arranging the groups of EditorLayout in a named layout and of flipping it.
#include "anvil/workbench/editor_layout.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>

using namespace anvil::workbench;
using anvil::core::Uri;

namespace {

Uri doc(const std::string& name) {
    return Uri{"file", "", "/work/" + name, "", ""};
}

LayoutShape group() {
    return {};
}
LayoutShape split(std::vector<LayoutShape> children) {
    return {std::move(children)};
}

struct Layouts {
    Orientation orientation;
    std::vector<LayoutShape> children;
};

// The layouts of VS Code's View > Editor Layout, as `workbench.action.editorLayout*` define them.
Layouts single() {
    return {Orientation::horizontal, {group()}};
}
Layouts two_columns() {
    return {Orientation::horizontal, {group(), group()}};
}
Layouts three_columns() {
    return {Orientation::horizontal, {group(), group(), group()}};
}
Layouts two_rows() {
    return {Orientation::vertical, {group(), group()}};
}
Layouts three_rows() {
    return {Orientation::vertical, {group(), group(), group()}};
}
Layouts grid() {
    return {Orientation::horizontal, {split({group(), group()}), split({group(), group()})}};
}
Layouts two_columns_bottom() {
    return {Orientation::vertical, {group(), split({group(), group()})}};
}
Layouts two_rows_right() {
    return {Orientation::horizontal, {group(), split({group(), group()})}};
}

// The tree as text: H(...) and V(...) for splitters, the number of the group in the order shown for
// a group.
std::string describe(const LayoutNode& node, const std::vector<GroupId>& order) {
    if (node.group) {
        return std::to_string(std::find(order.begin(), order.end(), *node.group) - order.begin() +
                              1);
    }
    std::string text = node.orientation == Orientation::horizontal ? "H(" : "V(";
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        text += (i ? "," : "") + describe(node.children[i], order);
    }
    return text + ")";
}

std::string shape_of(const EditorLayout& layout) {
    return describe(layout.root(), layout.group_ids());
}

void apply(EditorLayout& layout, const Layouts& arrangement) {
    layout.apply_layout(arrangement.orientation, arrangement.children);
}

// A layout of `count` groups side by side, group n holding the tabs "n.a", "n.b".
EditorLayout columns(std::size_t count) {
    EditorLayout layout;
    GroupId last = layout.focused();
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            last = *layout.split(last, Direction::right);
        }
        layout.open(last, doc(std::to_string(i + 1) + ".a"));
        layout.open(last, doc(std::to_string(i + 1) + ".b"));
    }
    return layout;
}

std::vector<std::string> tabs_of(const EditorLayout& layout, std::size_t group_number) {
    std::vector<std::string> result;
    for (const Uri& uri : layout.group(layout.group_ids()[group_number])->tabs) {
        result.push_back(uri.path.substr(6));
    }
    return result;
}

class Recorder : public EditorLayout::Observer {
public:
    void structure_changed() override {
        ++structure;
    }
    int structure = 0;
};

} // namespace

TEST(EditorLayoutArrangement, EachLayoutIsVSCodesTree) {
    struct Case {
        Layouts layout;
        const char* tree;
    };
    const Case cases[] = {
        {single(), "1"},
        {two_columns(), "H(1,2)"},
        {three_columns(), "H(1,2,3)"},
        {two_rows(), "V(1,2)"},
        {three_rows(), "V(1,2,3)"},
        {grid(), "H(V(1,2),V(3,4))"},
        {two_columns_bottom(), "V(1,H(2,3))"},
        {two_rows_right(), "H(1,V(2,3))"},
    };
    for (const Case& c : cases) {
        for (const std::size_t start : {1u, 2u, 3u, 5u}) {
            EditorLayout layout = columns(start);
            apply(layout, c.layout);
            EXPECT_EQ(shape_of(layout), c.tree) << c.tree << " from " << start << " groups";
            EXPECT_EQ(layout.find_inconsistency(), std::nullopt)
                << c.tree << " from " << start << " groups";
        }
    }
}

TEST(EditorLayoutArrangement, EveryShareOfASplitterIsEqual) {
    EditorLayout layout = columns(1);
    apply(layout, grid());
    ASSERT_FALSE(layout.root().is_group());
    EXPECT_EQ(layout.root().ratios, (std::vector<double>{0.5, 0.5}));
    EXPECT_EQ(layout.root().children[0].ratios, (std::vector<double>{0.5, 0.5}));
    apply(layout, three_rows());
    for (const double ratio : layout.root().ratios) {
        EXPECT_NEAR(ratio, 1.0 / 3, 1e-12);
    }
}

TEST(EditorLayoutArrangement, MoreGroupsAreEmptyAndTheOnesThatExistKeepTheirTabsAndPlaces) {
    EditorLayout layout = columns(2);
    const auto before = layout.group_ids();
    apply(layout, grid());
    const auto after = layout.group_ids();
    ASSERT_EQ(after.size(), 4u);
    EXPECT_EQ(after[0], before[0]);
    EXPECT_EQ(after[1], before[1]);
    EXPECT_EQ(tabs_of(layout, 0), (std::vector<std::string>{"1.a", "1.b"}));
    EXPECT_EQ(tabs_of(layout, 1), (std::vector<std::string>{"2.a", "2.b"}));
    EXPECT_TRUE(tabs_of(layout, 2).empty());
    EXPECT_TRUE(tabs_of(layout, 3).empty());
}

TEST(EditorLayoutArrangement, FewerGroupsJoinTheTabsOfTheOnesThatGoToTheLastOneThatStays) {
    EditorLayout layout = columns(4);
    layout.focus(layout.group_ids()[0]);
    apply(layout, two_columns());
    ASSERT_EQ(layout.group_ids().size(), 2u);
    EXPECT_EQ(tabs_of(layout, 0), (std::vector<std::string>{"1.a", "1.b"}));
    EXPECT_EQ(tabs_of(layout, 1),
              (std::vector<std::string>{"2.a", "2.b", "3.a", "3.b", "4.a", "4.b"}));
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);
}

TEST(EditorLayoutArrangement, ADocumentThatTheLastGroupHasAlreadyIsNotAddedTwice) {
    EditorLayout layout = columns(2);
    layout.open(layout.group_ids()[0], doc("2.a"));
    apply(layout, single());
    EXPECT_EQ(tabs_of(layout, 0), (std::vector<std::string>{"1.a", "1.b", "2.a", "2.b"}));
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);
}

TEST(EditorLayoutArrangement, TheFocusStaysUnlessItsGroupGoesThenTheLastGroupThatStaysHasIt) {
    EditorLayout layout = columns(3);
    const auto ids = layout.group_ids();
    layout.focus(ids[0]);
    apply(layout, two_rows());
    EXPECT_EQ(layout.focused(), ids[0]);

    EditorLayout other = columns(3);
    const auto others = other.group_ids();
    other.focus(others[2]);
    other.activate(others[2], 1);
    apply(other, two_rows());
    EXPECT_EQ(other.focused(), others[1]);
    const EditorGroup& target = *other.group(others[1]);
    EXPECT_EQ(target.tabs[*target.active].path, "/work/3.b") << "what was in front stays in front";
}

TEST(EditorLayoutArrangement, TheGroupsAreReportedAsAChangeOfTheStructure) {
    EditorLayout layout = columns(2);
    Recorder recorder;
    layout.add_observer(&recorder);
    apply(layout, three_columns());
    EXPECT_GE(recorder.structure, 1);
}

TEST(EditorLayoutArrangement, ApplyingTheSameLayoutAgainKeepsTheTabs) {
    EditorLayout layout = columns(3);
    apply(layout, two_rows_right());
    const auto first = shape_of(layout);
    apply(layout, two_rows_right());
    EXPECT_EQ(shape_of(layout), first);
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);
    EXPECT_EQ(tabs_of(layout, 0), (std::vector<std::string>{"1.a", "1.b"}));
}

TEST(EditorLayoutFlip, TurnsColumnsIntoRowsAndBackKeepingOrderAndShares) {
    EditorLayout layout = columns(3);
    ASSERT_TRUE(layout.set_ratios({}, {1, 2, 3}));
    const auto ratios = layout.root().ratios;
    ASSERT_TRUE(layout.flip());
    EXPECT_EQ(shape_of(layout), "V(1,2,3)");
    EXPECT_EQ(layout.root().ratios, ratios);
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);
    ASSERT_TRUE(layout.flip());
    EXPECT_EQ(shape_of(layout), "H(1,2,3)");
    EXPECT_EQ(layout.root().ratios, ratios);
}

TEST(EditorLayoutFlip, FlipsNestedSplittersToo) {
    EditorLayout layout = columns(1);
    apply(layout, two_rows_right());
    ASSERT_TRUE(layout.flip());
    EXPECT_EQ(shape_of(layout), "V(1,H(2,3))");
    EXPECT_EQ(layout.find_inconsistency(), std::nullopt);
}

TEST(EditorLayoutFlip, DoesNothingForASingleGroup) {
    EditorLayout layout;
    EXPECT_FALSE(layout.flip());
}

TEST(EditorLayoutArrangement, RandomArrangementsAndFlipsKeepTheModelConsistentAndLoseNoDocument) {
    std::mt19937 engine(4);
    const auto below = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(engine);
    };
    const std::vector<Layouts (*)()> all = {
        single,     two_columns, three_columns,      two_rows,
        three_rows, grid,        two_columns_bottom, two_rows_right};
    for (int round = 0; round < 800; ++round) {
        EditorLayout layout;
        for (std::size_t i = below(5); i > 0; --i) {
            const auto ids = layout.group_ids();
            layout.split(ids[below(ids.size())], static_cast<Direction>(below(4)));
        }
        std::set<std::string> documents;
        for (const GroupId id : layout.group_ids()) {
            for (std::size_t i = below(4); i > 0; --i) {
                const std::string name = std::to_string(below(6));
                layout.open(id, doc(name));
                documents.insert(name);
            }
        }
        for (int step = 0; step < 4; ++step) {
            if (below(3) == 0) {
                layout.flip();
            } else {
                apply(layout, all[below(all.size())]());
            }
            ASSERT_EQ(layout.find_inconsistency(), std::nullopt) << "round " << round;
            std::set<std::string> now;
            for (const GroupId id : layout.group_ids()) {
                for (const Uri& uri : layout.group(id)->tabs) {
                    now.insert(uri.path.substr(6));
                }
            }
            ASSERT_EQ(now, documents) << "round " << round << " step " << step;
        }
    }
}
