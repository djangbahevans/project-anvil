// Tests of moving between tabs and between groups in EditorLayout: the order of the tabs, the
// history of use, and the neighbors of a group by where the tree puts it.
#include "anvil/workbench/editor_layout.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <set>
#include <string>

using namespace anvil::workbench;
using anvil::core::Uri;

namespace {

Uri doc(const std::string& name) {
    return Uri{"file", "", "/work/" + name, "", ""};
}

std::string active_name(const EditorLayout& layout, GroupId group) {
    const EditorGroup* g = layout.group(group);
    return g && g->active ? g->tabs[*g->active].path.substr(6) : "-";
}

void open(EditorLayout& layout, GroupId group, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        ASSERT_TRUE(layout.open(group, doc(name)).has_value());
    }
}

// Two groups side by side, "a b c" in the first and "d e" in the second, the first focused.
struct TwoGroups {
    TwoGroups() {
        first = layout.focused();
        open(layout, first, {"a", "b", "c"});
        second = *layout.split(first, Direction::right);
        open(layout, second, {"d", "e"});
        layout.focus(first);
        layout.activate(first, 0);
    }
    EditorLayout layout;
    GroupId first{};
    GroupId second{};
};

} // namespace

TEST(EditorLayoutNextEditor, GoesToTheNextTabOfTheGroupThenToTheNextGroupAndWraps) {
    TwoGroups g;
    std::string seen;
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(g.layout.next_editor(true));
        seen += active_name(g.layout, g.layout.focused()) + " ";
    }
    EXPECT_EQ(seen, "b c d e a b ");
}

TEST(EditorLayoutNextEditor, PreviousGoesBackAndWrapsToTheLastTabOfTheLastGroup) {
    TwoGroups g;
    std::string seen;
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(g.layout.next_editor(false));
        seen += active_name(g.layout, g.layout.focused()) + " ";
    }
    EXPECT_EQ(seen, "e d c b a e ");
}

TEST(EditorLayoutNextEditor, SkipsGroupsWithoutTabsAndStaysInAGroupThatIsAlone) {
    EditorLayout layout;
    const GroupId first = layout.focused();
    open(layout, first, {"a", "b"});
    const GroupId empty = *layout.split(first, Direction::right);
    layout.focus(first);
    layout.activate(first, 1);
    ASSERT_TRUE(layout.next_editor(true));
    EXPECT_EQ(layout.focused(), first);
    EXPECT_EQ(active_name(layout, first), "a") << "the empty group has nothing to show";
    EXPECT_NE(layout.focused(), empty);
}

TEST(EditorLayoutNextEditor, FromAnEmptyGroupItShowsTheNextOrPreviousGroupsTab) {
    TwoGroups g;
    const GroupId third = *g.layout.split(g.second, Direction::right);
    ASSERT_EQ(g.layout.focused(), third);
    ASSERT_TRUE(g.layout.next_editor(true));
    EXPECT_EQ(g.layout.focused(), g.first);
    EXPECT_EQ(active_name(g.layout, g.first), "a");
    g.layout.focus(third);
    ASSERT_TRUE(g.layout.next_editor(false));
    EXPECT_EQ(g.layout.focused(), g.second);
    EXPECT_EQ(active_name(g.layout, g.second), "e");
}

TEST(EditorLayoutNextEditor, DoesNothingWithoutTabs) {
    EditorLayout layout;
    EXPECT_FALSE(layout.next_editor(true));
    EXPECT_FALSE(layout.next_editor(false));
    EXPECT_FALSE(layout.next_editor_in_group(true));
}

TEST(EditorLayoutNextEditorInGroup, WrapsInsideTheFocusedGroup) {
    TwoGroups g;
    std::string seen;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(g.layout.next_editor_in_group(true));
        seen += active_name(g.layout, g.first) + " ";
    }
    EXPECT_EQ(seen, "b c a b ");
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(g.layout.next_editor_in_group(false));
    }
    EXPECT_EQ(active_name(g.layout, g.first), "b");
    EXPECT_EQ(g.layout.focused(), g.first);
    EXPECT_EQ(active_name(g.layout, g.second), "e") << "the other group is untouched";
}

TEST(EditorLayoutRecent, OlderGoesBackThroughTheTabsInTheOrderOfUse) {
    EditorLayout layout;
    const GroupId group = layout.focused();
    open(layout, group, {"a", "b", "c", "d"});
    layout.activate(group, 1);
    layout.activate(group, 3);
    // Used last to first: d (0 of "a b c d" order: d is index 3), b, c, a.
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "b");
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "c")
        << "the history is the one when the first call began";
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "a");
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "a") << "stops at the oldest";
}

TEST(EditorLayoutRecent, NewerWalksBackTowardsTheTabThatWasShown) {
    EditorLayout layout;
    const GroupId group = layout.focused();
    open(layout, group, {"a", "b", "c"});
    ASSERT_TRUE(layout.navigate_recent(true, false));
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "a");
    ASSERT_TRUE(layout.navigate_recent(false, false));
    EXPECT_EQ(active_name(layout, group), "b");
    ASSERT_TRUE(layout.navigate_recent(false, false));
    EXPECT_EQ(active_name(layout, group), "c");
    ASSERT_TRUE(layout.navigate_recent(false, false));
    EXPECT_EQ(active_name(layout, group), "c") << "stops at the newest";
}

TEST(EditorLayoutRecent, AnyOtherChangeStartsANewHistory) {
    EditorLayout layout;
    const GroupId group = layout.focused();
    open(layout, group, {"a", "b", "c"});
    ASSERT_TRUE(layout.navigate_recent(true, false));
    ASSERT_EQ(active_name(layout, group), "b");
    layout.activate(group, 0); // the user picks "a"
    // The history is now a, b, c... : the one before "a" is "b" again, from a new start.
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "b");
    ASSERT_TRUE(layout.navigate_recent(true, false));
    EXPECT_EQ(active_name(layout, group), "c");
}

TEST(EditorLayoutRecent, AcrossGroupsTheGroupThatHadTheFocusLastComesFirst) {
    TwoGroups g;
    g.layout.focus(g.second);
    g.layout.focus(g.first);
    // First group "a b c" (a active, most recent a, b? order of recent: a, c, b), second "d e".
    ASSERT_TRUE(g.layout.navigate_recent(true, false));
    const std::string first_step = active_name(g.layout, g.layout.focused());
    EXPECT_EQ(g.layout.focused(), g.first) << "the next older tab is in the same group";
    EXPECT_EQ(first_step, "c");
    ASSERT_TRUE(g.layout.navigate_recent(true, false));
    ASSERT_TRUE(g.layout.navigate_recent(true, false));
    EXPECT_EQ(g.layout.focused(), g.second);
    EXPECT_EQ(active_name(g.layout, g.second), "e");
}

TEST(EditorLayoutRecent, InGroupOnlyTheFocusedGroupsTabsAreInTheHistory) {
    TwoGroups g;
    ASSERT_TRUE(g.layout.navigate_recent(true, true));
    EXPECT_EQ(g.layout.focused(), g.first);
    ASSERT_TRUE(g.layout.navigate_recent(true, true));
    ASSERT_TRUE(g.layout.navigate_recent(true, true));
    EXPECT_EQ(g.layout.focused(), g.first);
    EXPECT_EQ(active_name(g.layout, g.second), "e");
}

TEST(EditorLayoutRecent, DoesNothingWithoutTabs) {
    EditorLayout layout;
    EXPECT_FALSE(layout.navigate_recent(true, false));
    EXPECT_FALSE(layout.navigate_recent(false, true));
}

TEST(EditorLayoutFocusAt, FocusesTheGroupAtAnIndexInTheOrderTheyAreShown) {
    TwoGroups g;
    EXPECT_TRUE(g.layout.focus_at(1));
    EXPECT_EQ(g.layout.focused(), g.second);
    EXPECT_TRUE(g.layout.focus_at(0));
    EXPECT_EQ(g.layout.focused(), g.first);
    EXPECT_FALSE(g.layout.focus_at(2));
    EXPECT_EQ(g.layout.focused(), g.first);
}

TEST(EditorLayoutNeighbor, FindsTheGroupAcrossAnEdgeAndWrapsAtTheEdgeOfTheArea) {
    EditorLayout layout;
    const GroupId left = layout.focused();
    const GroupId right = *layout.split(left, Direction::right);
    EXPECT_EQ(layout.neighbor(left, Direction::right), right);
    EXPECT_EQ(layout.neighbor(right, Direction::left), left);
    EXPECT_EQ(layout.neighbor(right, Direction::right), left) << "wraps";
    EXPECT_EQ(layout.neighbor(left, Direction::left), right) << "wraps";
    EXPECT_EQ(layout.neighbor(left, Direction::up), left) << "alone in its column";
    EXPECT_EQ(layout.neighbor(left, Direction::down), left);
    EXPECT_EQ(layout.neighbor(GroupId{99}, Direction::left), std::nullopt);
}

TEST(EditorLayoutNeighbor, UsesTheGeometryOfTheTree) {
    // Left | (top right over bottom right)
    EditorLayout layout;
    const GroupId left = layout.focused();
    const GroupId top = *layout.split(left, Direction::right);
    const GroupId bottom = *layout.split(top, Direction::down);
    EXPECT_EQ(layout.neighbor(top, Direction::down), bottom);
    EXPECT_EQ(layout.neighbor(bottom, Direction::up), top);
    EXPECT_EQ(layout.neighbor(bottom, Direction::down), top) << "wraps";
    EXPECT_EQ(layout.neighbor(top, Direction::left), left);
    EXPECT_EQ(layout.neighbor(bottom, Direction::left), left);
    EXPECT_EQ(layout.neighbor(left, Direction::right), bottom)
        << "both touch it, and the last one focused is the bottom one";
    layout.focus(top);
    EXPECT_EQ(layout.neighbor(left, Direction::right), top);
}

TEST(EditorLayoutNeighbor, FocusNeighborMovesTheFocusAndReportsWhetherItMoved) {
    EditorLayout layout;
    const GroupId left = layout.focused();
    const GroupId right = *layout.split(left, Direction::right);
    layout.focus(left);
    EXPECT_TRUE(layout.focus_neighbor(Direction::right));
    EXPECT_EQ(layout.focused(), right);
    EXPECT_FALSE(layout.focus_neighbor(Direction::up));
    EXPECT_EQ(layout.focused(), right);
}

TEST(EditorLayoutNeighbor, RatiosDoNotChangeWhoTouchesWhom) {
    EditorLayout layout;
    const GroupId left = layout.focused();
    const GroupId right = *layout.split(left, Direction::right);
    const GroupId lower = *layout.split(right, Direction::down);
    ASSERT_TRUE(layout.set_ratios({}, {0.2, 0.8}));
    ASSERT_TRUE(layout.set_ratios({1}, {0.9, 0.1}));
    EXPECT_EQ(layout.neighbor(left, Direction::right), lower) << "the one focused last";
    layout.focus(right);
    EXPECT_EQ(layout.neighbor(left, Direction::right), right);
}

// --- model-based ---

namespace {

struct Rect {
    double left, top, right, bottom;
};

void rectangles(const LayoutNode& node, Rect rect, std::map<GroupId, Rect>& out) {
    if (node.group) {
        out[*node.group] = rect;
        return;
    }
    double at = 0;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        Rect child = rect;
        if (node.orientation == Orientation::horizontal) {
            child.left = rect.left + at * (rect.right - rect.left);
            child.right = rect.left + (at + node.ratios[i]) * (rect.right - rect.left);
        } else {
            child.top = rect.top + at * (rect.bottom - rect.top);
            child.bottom = rect.top + (at + node.ratios[i]) * (rect.bottom - rect.top);
        }
        at += node.ratios[i];
        rectangles(node.children[i], child, out);
    }
}

// The group that covers the point, or none.
std::optional<GroupId> at_point(const std::map<GroupId, Rect>& rects, double x, double y) {
    for (const auto& [id, r] : rects) {
        if (r.left <= x && x < r.right && r.top <= y && y < r.bottom) {
            return id;
        }
    }
    return std::nullopt;
}

TEST(EditorLayoutNeighbor, AgreesWithLookingAtPointsBesideTheGroupOnRandomLayouts) {
    std::mt19937 engine(31);
    const auto below = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(engine);
    };
    for (int round = 0; round < 400; ++round) {
        EditorLayout layout;
        const std::size_t splits = 1 + below(7);
        for (std::size_t i = 0; i < splits; ++i) {
            const auto ids = layout.group_ids();
            layout.split(ids[below(ids.size())], static_cast<Direction>(below(4)));
        }
        // Different sizes, so that edges do not line up by chance.
        for (int tweak = 0; tweak < 3; ++tweak) {
            std::vector<std::size_t> path;
            const LayoutNode* node = &layout.root();
            while (!node->group && below(2) == 0) {
                const std::size_t child = below(node->children.size());
                path.push_back(child);
                node = &node->children[child];
            }
            if (!node->group) {
                std::vector<double> ratios;
                for (std::size_t i = 0; i < node->children.size(); ++i) {
                    ratios.push_back(1.0 + static_cast<double>(below(5)));
                }
                layout.set_ratios(path, ratios);
            }
        }
        const auto ids = layout.group_ids();
        for (std::size_t i = 0; i < 4; ++i) {
            layout.focus(ids[below(ids.size())]);
        }
        const auto recency = layout.groups_by_recency();

        std::map<GroupId, Rect> rects;
        rectangles(layout.root(), Rect{0, 0, 1, 1}, rects);
        for (const GroupId id : ids) {
            const Rect me = rects.at(id);
            for (const Direction direction :
                 {Direction::left, Direction::right, Direction::up, Direction::down}) {
                // Points just outside the edge, one in the middle of each stretch of the edge that
                // the other groups' edges cut it into; beyond the area, the opposite side.
                std::set<double> cuts{
                    (direction == Direction::left || direction == Direction::right) ? me.top
                                                                                    : me.left,
                    (direction == Direction::left || direction == Direction::right) ? me.bottom
                                                                                    : me.right};
                for (const auto& [other, r] : rects) {
                    const bool across =
                        direction == Direction::left || direction == Direction::right;
                    for (const double cut : across ? std::vector<double>{r.top, r.bottom}
                                                   : std::vector<double>{r.left, r.right}) {
                        const double low = across ? me.top : me.left;
                        const double high = across ? me.bottom : me.right;
                        if (cut > low + 1e-9 && cut < high - 1e-9) {
                            cuts.insert(cut);
                        }
                    }
                }
                std::vector<double> sorted(cuts.begin(), cuts.end());
                std::set<GroupId> found;
                for (std::size_t k = 0; k + 1 < sorted.size(); ++k) {
                    const double along = (sorted[k] + sorted[k + 1]) / 2;
                    double x = 0;
                    double y = 0;
                    switch (direction) {
                    case Direction::right:
                        x = me.right >= 1 - 1e-9 ? 0.0 + 1e-7 : me.right + 1e-7;
                        y = along;
                        break;
                    case Direction::left:
                        x = me.left <= 1e-9 ? 1.0 - 1e-7 : me.left - 1e-7;
                        y = along;
                        break;
                    case Direction::down:
                        y = me.bottom >= 1 - 1e-9 ? 0.0 + 1e-7 : me.bottom + 1e-7;
                        x = along;
                        break;
                    case Direction::up:
                        y = me.top <= 1e-9 ? 1.0 - 1e-7 : me.top - 1e-7;
                        x = along;
                        break;
                    }
                    if (const auto hit = at_point(rects, x, y)) {
                        found.insert(*hit);
                    }
                }
                std::optional<GroupId> expected;
                for (const GroupId candidate : recency) {
                    if (found.contains(candidate)) {
                        expected = candidate;
                        break;
                    }
                }
                ASSERT_EQ(layout.neighbor(id, direction), expected)
                    << "round " << round << " group " << static_cast<unsigned>(id) << " direction "
                    << static_cast<int>(direction);
            }
        }
    }
}

TEST(EditorLayoutNextEditor, AgreesWithTheListOfAllTabsOnRandomLayouts) {
    std::mt19937 engine(8);
    const auto below = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(engine);
    };
    for (int round = 0; round < 1500; ++round) {
        EditorLayout layout;
        const std::size_t splits = below(4);
        for (std::size_t i = 0; i < splits; ++i) {
            const auto ids = layout.group_ids();
            layout.split(ids[below(ids.size())], static_cast<Direction>(below(4)));
        }
        const auto ids = layout.group_ids();
        for (const GroupId group : ids) {
            for (std::size_t i = 0, n = below(4); i < n; ++i) {
                layout.open(group, doc(std::to_string(below(6))));
            }
        }
        layout.focus(ids[below(ids.size())]);

        // All tabs in the order shown, and the position the focused group's active tab has in it,
        // or the place an empty focused group would have.
        std::vector<TabLocation> all;
        std::size_t position = 0;
        bool has_active = false;
        for (const GroupId group : ids) {
            const EditorGroup& g = *layout.group(group);
            if (group == layout.focused()) {
                position = all.size() + (g.active ? *g.active : 0);
                has_active = g.active.has_value();
            }
            for (std::size_t i = 0; i < g.tabs.size(); ++i) {
                all.push_back({group, i});
            }
        }
        const bool forward = below(2) == 0;
        TabLocation expected;
        if (all.empty()) {
            ASSERT_FALSE(layout.next_editor(forward)) << "round " << round;
            continue;
        }
        if (has_active) {
            expected = all[forward ? (position + 1) % all.size()
                                   : (position + all.size() - 1) % all.size()];
        } else {
            // An empty group: the first tab after it, or the last tab before it.
            expected =
                all[forward ? position % all.size() : (position + all.size() - 1) % all.size()];
        }
        ASSERT_TRUE(layout.next_editor(forward)) << "round " << round;
        ASSERT_EQ(layout.focused(), expected.group) << "round " << round;
        ASSERT_EQ(layout.group(expected.group)->active, expected.index) << "round " << round;
        ASSERT_EQ(layout.find_inconsistency(), std::nullopt) << "round " << round;
    }
}

} // namespace
