// Model-based test of EditorLayout: random operations are mirrored by a naive reference that holds
// the groups in the order they are shown, and after every step the layout must match it and keep
// the rules of its tree.
#include "anvil/workbench/editor_layout.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <string>

using namespace anvil::workbench;
using anvil::core::Uri;

namespace {

Uri doc(std::size_t number) {
    return Uri{"file", "", "/work/" + std::to_string(number), "", ""};
}

struct RefGroup {
    std::vector<Uri> tabs;
    std::optional<std::size_t> active;
};

// The semantics of EditorLayout without the tree: the groups in display order, each with its tabs.
struct Reference {
    bool close_empty = true;
    std::vector<GroupId> order;
    std::map<GroupId, RefGroup> groups;
    GroupId focused{};

    std::optional<std::size_t> find(GroupId group, const Uri& uri) const {
        const auto& tabs = groups.at(group).tabs;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i] == uri) {
                return i;
            }
        }
        return std::nullopt;
    }

    void open(GroupId group, const Uri& uri) {
        RefGroup& g = groups.at(group);
        if (const auto existing = find(group, uri)) {
            g.active = existing;
            return;
        }
        const std::size_t at = g.active ? *g.active + 1 : g.tabs.size();
        g.tabs.insert(g.tabs.begin() + static_cast<std::ptrdiff_t>(at), uri);
        g.active = at;
    }

    void drop(GroupId group, std::size_t index) {
        RefGroup& g = groups.at(group);
        const std::size_t active = *g.active;
        g.tabs.erase(g.tabs.begin() + static_cast<std::ptrdiff_t>(index));
        if (g.tabs.empty()) {
            g.active.reset();
        } else if (index == active) {
            g.active = index < g.tabs.size() ? index : g.tabs.size() - 1;
        } else if (index < active) {
            g.active = active - 1;
        }
    }

    void erase_group(GroupId group) {
        const auto at = std::ranges::find(order, group) - order.begin();
        if (focused == group) {
            focused = at > 0 ? order[static_cast<std::size_t>(at - 1)]
                             : order[static_cast<std::size_t>(at + 1)];
        }
        order.erase(order.begin() + at);
        groups.erase(group);
    }

    void close_if_empty(GroupId group) {
        if (close_empty && groups.at(group).tabs.empty() && order.size() > 1) {
            erase_group(group);
        }
    }

    void replace(const Uri& from, const Uri& to) {
        if (from == to) {
            return;
        }
        for (const GroupId group : std::vector<GroupId>(order)) {
            const auto old_index = find(group, from);
            if (!old_index) {
                continue;
            }
            if (find(group, to)) {
                drop(group, *old_index);
                close_if_empty(group);
            } else {
                groups.at(group).tabs[*old_index] = to;
            }
        }
    }

    void close_tab(GroupId group, std::size_t index) {
        drop(group, index);
        close_if_empty(group);
    }

    void move(GroupId from, std::size_t index, GroupId to, std::size_t to_index) {
        RefGroup& source = groups.at(from);
        const Uri moved = source.tabs[index];
        if (from == to) {
            const Uri active = source.tabs[*source.active];
            source.tabs.erase(source.tabs.begin() + static_cast<std::ptrdiff_t>(index));
            const std::size_t at = std::min(to_index, source.tabs.size());
            source.tabs.insert(source.tabs.begin() + static_cast<std::ptrdiff_t>(at), moved);
            source.active = find(from, active);
            return;
        }
        drop(from, index);
        RefGroup& target = groups.at(to);
        if (const auto existing = find(to, moved)) {
            target.active = existing;
        } else {
            const std::size_t at = std::min(to_index, target.tabs.size());
            target.tabs.insert(target.tabs.begin() + static_cast<std::ptrdiff_t>(at), moved);
            target.active = at;
        }
        focused = to;
        close_if_empty(from);
    }

    GroupId split(GroupId group, Direction direction, SplitContent content, GroupId created) {
        RefGroup added;
        const RefGroup& origin = groups.at(group);
        if (content == SplitContent::copy_active && origin.active) {
            added.tabs.push_back(origin.tabs[*origin.active]);
            added.active = 0;
        }
        const auto at = std::ranges::find(order, group) - order.begin();
        const bool after = direction == Direction::right || direction == Direction::down;
        order.insert(order.begin() + at + (after ? 1 : 0), created);
        groups[created] = added;
        focused = created;
        return created;
    }
};

class Random {
public:
    explicit Random(unsigned seed) : engine_(seed) {}
    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, bound - 1)(engine_);
    }

private:
    std::mt19937 engine_;
};

void expect_same(const EditorLayout& layout, const Reference& reference, const std::string& step) {
    ASSERT_EQ(layout.find_inconsistency(), std::nullopt) << step;
    ASSERT_EQ(layout.group_ids(), reference.order) << step;
    ASSERT_EQ(layout.focused(), reference.focused) << step;
    for (const GroupId id : reference.order) {
        const EditorGroup* group = layout.group(id);
        const RefGroup& expected = reference.groups.at(id);
        ASSERT_NE(group, nullptr) << step;
        ASSERT_EQ(group->tabs, expected.tabs) << step;
        ASSERT_EQ(group->active, expected.active) << step;
    }
}

void run(unsigned seed, bool close_empty) {
    Random random(seed);
    EditorLayout layout({.close_empty_groups = close_empty});
    Reference reference;
    reference.close_empty = close_empty;
    reference.order = layout.group_ids();
    reference.focused = layout.focused();
    reference.groups[reference.focused] = {};
    std::uint32_t last_id = static_cast<std::uint32_t>(reference.focused);

    constexpr std::size_t documents = 7;
    for (int step = 0; step < 400; ++step) {
        const GroupId group = reference.order[random.below(reference.order.size())];
        const GroupId other = reference.order[random.below(reference.order.size())];
        const RefGroup& g = reference.groups.at(group);
        const Uri uri = doc(random.below(documents));
        std::string what;
        const std::size_t kind = random.below(14);
        switch (kind) {
        case 0:
        case 1:
            what = "open";
            layout.open(group, uri);
            reference.open(group, uri);
            break;
        case 2: {
            what = "show";
            layout.show(uri);
            GroupId target = reference.focused;
            if (!reference.find(target, uri)) {
                for (const GroupId candidate : reference.order) {
                    if (reference.find(candidate, uri)) {
                        target = candidate;
                        break;
                    }
                }
            }
            reference.focused = target;
            reference.open(target, uri);
            break;
        }
        case 3:
            what = "activate";
            if (!g.tabs.empty()) {
                const std::size_t index = random.below(g.tabs.size());
                layout.activate(group, index);
                reference.groups.at(group).active = index;
            }
            break;
        case 4:
        case 5:
            what = "close_tab";
            if (!g.tabs.empty()) {
                const std::size_t index = random.below(g.tabs.size());
                layout.close_tab(group, index);
                reference.close_tab(group, index);
            }
            break;
        case 6:
        case 7:
            what = "move_tab";
            if (!g.tabs.empty()) {
                const std::size_t index = random.below(g.tabs.size());
                const std::size_t to = random.below(reference.groups.at(other).tabs.size() + 2);
                layout.move_tab(group, index, other, to);
                reference.move(group, index, other, to);
            }
            break;
        case 8:
            what = "split";
            if (reference.order.size() < 8) {
                const auto direction = static_cast<Direction>(random.below(4));
                const auto content =
                    random.below(2) ? SplitContent::copy_active : SplitContent::empty;
                const auto created = layout.split(group, direction, content);
                ASSERT_TRUE(created);
                ASSERT_EQ(static_cast<std::uint32_t>(*created), ++last_id);
                reference.split(group, direction, content, *created);
            }
            break;
        case 9:
            what = "close_group";
            if (reference.order.size() > 1) {
                ASSERT_TRUE(layout.close_group(group));
                reference.erase_group(group);
            }
            break;
        case 10:
            what = "focus";
            layout.focus(group);
            reference.focused = group;
            break;
        case 11: {
            what = "focus_next_or_previous";
            const auto at =
                std::ranges::find(reference.order, reference.focused) - reference.order.begin();
            const auto count = static_cast<std::ptrdiff_t>(reference.order.size());
            if (random.below(2)) {
                layout.focus_next();
                reference.focused = reference.order[static_cast<std::size_t>((at + 1) % count)];
            } else {
                layout.focus_previous();
                reference.focused =
                    reference.order[static_cast<std::size_t>((at + count - 1) % count)];
            }
            break;
        }
        case 12: {
            what = "replace_document";
            const Uri to = doc(random.below(documents));
            layout.replace_document(uri, to);
            reference.replace(uri, to);
            break;
        }
        default: {
            what = "set_ratios";
            std::vector<std::size_t> path;
            const LayoutNode* node = &layout.root();
            while (!node->is_group() && random.below(3) != 0) {
                const std::size_t child = random.below(node->children.size());
                path.push_back(child);
                node = &node->children[child];
            }
            if (!node->is_group()) {
                std::vector<double> ratios;
                for (std::size_t i = 0; i < node->children.size(); ++i) {
                    ratios.push_back(static_cast<double>(1 + random.below(9)));
                }
                ASSERT_TRUE(layout.set_ratios(path, ratios));
            }
            break;
        }
        }
        expect_same(layout, reference,
                    "seed " + std::to_string(seed) + " step " + std::to_string(step) + " " + what);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

} // namespace

TEST(EditorLayoutModelTest, RandomOperationsMatchTheReferenceAndKeepTheRules) {
    for (unsigned seed = 1; seed <= 60; ++seed) {
        run(seed, true);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

TEST(EditorLayoutModelTest, RandomOperationsMatchTheReferenceWhenEmptyGroupsStay) {
    for (unsigned seed = 100; seed <= 140; ++seed) {
        run(seed, false);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

TEST(EditorLayoutModelTest, ASavedLayoutIsRestoredToTheSameLayoutAtEveryStep) {
    Random random(7);
    EditorLayout layout;
    for (int step = 0; step < 300; ++step) {
        const std::vector<GroupId> ids = layout.group_ids();
        const GroupId group = ids[random.below(ids.size())];
        switch (random.below(6)) {
        case 0:
            layout.open(group, doc(random.below(6)));
            break;
        case 1:
            if (ids.size() < 6) {
                layout.split(group, static_cast<Direction>(random.below(4)),
                             random.below(2) ? SplitContent::copy_active : SplitContent::empty);
            }
            break;
        case 2:
            layout.close_group(group);
            break;
        case 3: {
            const EditorGroup* g = layout.group(group);
            if (!g->tabs.empty()) {
                layout.close_tab(group, random.below(g->tabs.size()));
            }
            break;
        }
        case 4: {
            const EditorGroup* g = layout.group(group);
            if (!g->tabs.empty()) {
                layout.move_tab(group, random.below(g->tabs.size()), ids[random.below(ids.size())],
                                random.below(5));
            }
            break;
        }
        default:
            layout.focus(group);
            break;
        }
        const auto restored = EditorLayout::from_json(layout.to_json());
        ASSERT_TRUE(restored) << "step " << step << ": " << restored.error().message;
        ASSERT_EQ(restored->to_json(), layout.to_json()) << "step " << step;
        ASSERT_EQ(restored->root(), layout.root()) << "step " << step;
    }
}
