#include "anvil/workbench/editor_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>

namespace anvil::workbench {

namespace {

constexpr double ratio_tolerance = 1e-6;

void collect_groups(const LayoutNode& node, std::vector<GroupId>& out) {
    if (node.group) {
        out.push_back(*node.group);
        return;
    }
    for (const LayoutNode& child : node.children) {
        collect_groups(child, out);
    }
}

bool find_path(const LayoutNode& node, GroupId id, std::vector<std::size_t>& path) {
    if (node.group) {
        return *node.group == id;
    }
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        path.push_back(i);
        if (find_path(node.children[i], id, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

LayoutNode* node_at(LayoutNode& root, const std::vector<std::size_t>& path) {
    LayoutNode* node = &root;
    for (const std::size_t index : path) {
        if (node->group || index >= node->children.size()) {
            return nullptr;
        }
        node = &node->children[index];
    }
    return node;
}

LayoutNode group_node(GroupId id) {
    LayoutNode node;
    node.group = id;
    return node;
}

LayoutNode splitter_of(Orientation orientation, LayoutNode first, LayoutNode second) {
    LayoutNode node;
    node.orientation = orientation;
    node.children.push_back(std::move(first));
    node.children.push_back(std::move(second));
    node.ratios = {0.5, 0.5};
    return node;
}

// Restores the shape the model keeps after children were removed: a splitter inside a splitter of
// the same orientation is merged into it, and a splitter with one child is replaced by the child.
void canonicalize(LayoutNode& node) {
    if (node.group) {
        return;
    }
    for (LayoutNode& child : node.children) {
        canonicalize(child);
    }
    std::vector<LayoutNode> children;
    std::vector<double> ratios;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        LayoutNode& child = node.children[i];
        if (!child.group && child.orientation == node.orientation) {
            for (std::size_t j = 0; j < child.children.size(); ++j) {
                children.push_back(std::move(child.children[j]));
                ratios.push_back(node.ratios[i] * child.ratios[j]);
            }
        } else {
            children.push_back(std::move(child));
            ratios.push_back(node.ratios[i]);
        }
    }
    node.children = std::move(children);
    node.ratios = std::move(ratios);
    if (node.children.size() == 1) {
        LayoutNode only = std::move(node.children.front());
        node = std::move(only);
    }
}

bool same_shape(const LayoutNode& a, const LayoutNode& b) {
    if (a.group != b.group || a.children.size() != b.children.size()) {
        return false;
    }
    if (!a.group && a.orientation != b.orientation) {
        return false;
    }
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!same_shape(a.children[i], b.children[i])) {
            return false;
        }
    }
    return true;
}

std::optional<core::Uri> active_uri(const EditorGroup& group) {
    if (!group.active) {
        return std::nullopt;
    }
    return group.tabs[*group.active];
}

// Makes the tab the active one, and the most recently used.
void activate_tab(EditorGroup& group, std::size_t index) {
    group.active = index;
    const core::Uri& uri = group.tabs[index];
    const auto found = std::ranges::find(group.recent, uri);
    if (found != group.recent.end()) {
        std::rotate(group.recent.begin(), found, found + 1);
    } else {
        group.recent.insert(group.recent.begin(), uri);
    }
}

std::optional<std::size_t> index_of(const EditorGroup& group, const core::Uri& uri) {
    const auto found = std::ranges::find(group.tabs, uri);
    if (found == group.tabs.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - group.tabs.begin());
}

// The part of the editor area a group takes, as fractions of its width and height.
struct Box {
    double left = 0;
    double top = 0;
    double right = 1;
    double bottom = 1;
};

void collect_boxes(const LayoutNode& node, const Box& box,
                   std::vector<std::pair<GroupId, Box>>& out) {
    if (node.group) {
        out.push_back({*node.group, box});
        return;
    }
    double offset = 0;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        Box child = box;
        if (node.orientation == Orientation::horizontal) {
            const double width = box.right - box.left;
            child.left = box.left + offset * width;
            child.right = box.left + (offset + node.ratios[i]) * width;
        } else {
            const double height = box.bottom - box.top;
            child.top = box.top + offset * height;
            child.bottom = box.top + (offset + node.ratios[i]) * height;
        }
        offset += node.ratios[i];
        collect_boxes(node.children[i], child, out);
    }
}

bool near(double a, double b) {
    return std::abs(a - b) < ratio_tolerance;
}

// Whether the two ranges share more than a point.
bool overlap(double first_begin, double first_end, double second_begin, double second_end) {
    return std::min(first_end, second_end) - std::max(first_begin, second_begin) > ratio_tolerance;
}

} // namespace

struct EditorLayout::Snapshot {
    LayoutNode root;
    std::vector<EditorGroup> groups;
    GroupId focused{};
};

EditorLayout::EditorLayout(EditorLayoutOptions options) : options_(options) {
    const GroupId first{next_id_++};
    root_ = group_node(first);
    groups_.push_back({.id = first, .tabs = {}, .active = std::nullopt, .recent = {}});
    focused_ = first;
    recent_groups_ = {first};
}

void EditorLayout::add_observer(Observer* observer) {
    observers_.push_back(observer);
}

void EditorLayout::remove_observer(Observer* observer) {
    std::erase(observers_, observer);
}

std::vector<GroupId> EditorLayout::group_ids() const {
    std::vector<GroupId> ids;
    collect_groups(root_, ids);
    return ids;
}

const EditorGroup* EditorLayout::group(GroupId id) const {
    const auto found = std::ranges::find(groups_, id, &EditorGroup::id);
    return found == groups_.end() ? nullptr : &*found;
}

EditorGroup* EditorLayout::mutable_group(GroupId id) {
    const auto found = std::ranges::find(groups_, id, &EditorGroup::id);
    return found == groups_.end() ? nullptr : &*found;
}

std::vector<TabLocation> EditorLayout::locations(const core::Uri& uri) const {
    std::vector<TabLocation> result;
    for (const GroupId id : group_ids()) {
        if (const auto index = index_of(*group(id), uri)) {
            result.push_back({id, *index});
        }
    }
    return result;
}

EditorLayout::Snapshot EditorLayout::snapshot() const {
    return {root_, groups_, focused_};
}

void EditorLayout::notify(const Snapshot& before) {
    if (before.focused != focused_) {
        std::erase(recent_groups_, focused_);
        recent_groups_.insert(recent_groups_.begin(), focused_);
    }
    std::erase_if(recent_groups_, [&](GroupId id) { return !group(id); });
    // The history of use that the calls of navigate_recent keep ends when the tabs or the focus
    // change by anything else.
    if (before.groups != groups_ || before.focused != focused_) {
        if (!navigating_all_) {
            recent_all_.reset();
        }
        if (!navigating_in_group_) {
            recent_in_group_.reset();
        }
    }
    if (observers_.empty()) {
        return;
    }
    // Observers may add or remove observers, or change the layout, while they are told.
    const std::vector<Observer*> observers = observers_;
    const auto tell = [&](auto&& call) {
        for (Observer* observer : observers) {
            call(*observer);
        }
    };

    if (!same_shape(before.root, root_)) {
        tell([](Observer& o) { o.structure_changed(); });
    } else if (before.root != root_) {
        tell([](Observer& o) { o.ratios_changed(); });
    }
    const std::vector<EditorGroup> now = groups_;
    for (const EditorGroup& current : now) {
        const auto was = std::ranges::find(before.groups, current.id, &EditorGroup::id);
        if (was == before.groups.end()) {
            continue;
        }
        if (was->tabs != current.tabs) {
            tell([&](Observer& o) { o.tabs_changed(current.id); });
        }
        if (active_uri(*was) != active_uri(current)) {
            tell([&](Observer& o) { o.active_tab_changed(current.id); });
        }
    }
    if (before.focused != focused_) {
        const GroupId now_focused = focused_;
        tell([&](Observer& o) { o.focus_changed(now_focused); });
    }
}

std::optional<std::size_t> EditorLayout::open_in(EditorGroup& target, const core::Uri& uri) {
    if (const auto existing = index_of(target, uri)) {
        activate_tab(target, *existing);
        return existing;
    }
    const std::size_t position = target.active ? *target.active + 1 : target.tabs.size();
    target.tabs.insert(target.tabs.begin() + static_cast<std::ptrdiff_t>(position), uri);
    activate_tab(target, position);
    return position;
}

void EditorLayout::remove_tab(EditorGroup& target, std::size_t index) const {
    const bool was_active = target.active && index == *target.active;
    std::erase(target.recent, target.tabs[index]);
    target.tabs.erase(target.tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (target.tabs.empty()) {
        target.active = std::nullopt;
    } else if (was_active && options_.focus_recent_after_close) {
        activate_tab(target, *index_of(target, target.recent.front()));
    } else if (was_active) {
        activate_tab(target, std::min(index, target.tabs.size() - 1));
    } else if (index < *target.active) {
        target.active = *target.active - 1;
    }
}

std::optional<std::size_t> EditorLayout::open(GroupId id, const core::Uri& uri) {
    EditorGroup* target = mutable_group(id);
    if (!target) {
        return std::nullopt;
    }
    const Snapshot before = snapshot();
    const auto position = open_in(*target, uri);
    notify(before);
    return position;
}

TabLocation EditorLayout::show(const core::Uri& uri) {
    const Snapshot before = snapshot();
    TabLocation location;
    const auto locations_of_uri = locations(uri);
    const auto in_focused = std::ranges::find(locations_of_uri, focused_, &TabLocation::group);
    if (in_focused != locations_of_uri.end()) {
        location = *in_focused;
    } else if (!locations_of_uri.empty()) {
        location = locations_of_uri.front();
        focused_ = location.group;
    } else {
        location = {focused_, 0};
        location.index = *open_in(*mutable_group(focused_), uri);
    }
    activate_tab(*mutable_group(location.group), location.index);
    notify(before);
    return location;
}

bool EditorLayout::activate(GroupId id, std::size_t index) {
    EditorGroup* target = mutable_group(id);
    if (!target || index >= target->tabs.size()) {
        return false;
    }
    const Snapshot before = snapshot();
    activate_tab(*target, index);
    notify(before);
    return true;
}

bool EditorLayout::close_tab(GroupId id, std::size_t index) {
    EditorGroup* target = mutable_group(id);
    if (!target || index >= target->tabs.size()) {
        return false;
    }
    const Snapshot before = snapshot();
    remove_tab(*target, index);
    close_group_if_empty(id);
    notify(before);
    return true;
}

void EditorLayout::close_group_if_empty(GroupId id) {
    const EditorGroup* target = group(id);
    if (options_.close_empty_groups && target && target->tabs.empty() && groups_.size() > 1) {
        erase_group(id);
    }
}

bool EditorLayout::move_tab(GroupId from, std::size_t from_index, GroupId to,
                            std::size_t to_index) {
    EditorGroup* source = mutable_group(from);
    EditorGroup* destination = mutable_group(to);
    if (!source || !destination || from_index >= source->tabs.size()) {
        return false;
    }
    const Snapshot before = snapshot();
    if (from == to) {
        const core::Uri active = *active_uri(*source);
        const core::Uri moved = source->tabs[from_index];
        source->tabs.erase(source->tabs.begin() + static_cast<std::ptrdiff_t>(from_index));
        const std::size_t target = std::min(to_index, source->tabs.size());
        source->tabs.insert(source->tabs.begin() + static_cast<std::ptrdiff_t>(target), moved);
        source->active = index_of(*source, active);
    } else {
        const core::Uri moved = source->tabs[from_index];
        remove_tab(*source, from_index);
        if (const auto existing = index_of(*destination, moved)) {
            activate_tab(*destination, *existing);
        } else {
            const std::size_t position = std::min(to_index, destination->tabs.size());
            destination->tabs.insert(
                destination->tabs.begin() + static_cast<std::ptrdiff_t>(position), moved);
            activate_tab(*destination, position);
        }
        focused_ = to;
        close_group_if_empty(from);
    }
    notify(before);
    return true;
}

std::optional<GroupId> EditorLayout::split(GroupId id, Direction direction, SplitContent content) {
    EditorGroup* origin = mutable_group(id);
    std::vector<std::size_t> path;
    if (!origin || !find_path(root_, id, path)) {
        return std::nullopt;
    }
    const Snapshot before = snapshot();

    const GroupId created{next_id_++};
    EditorGroup added{.id = created, .tabs = {}, .active = std::nullopt, .recent = {}};
    if (content == SplitContent::copy_active && origin->active) {
        added.tabs.push_back(origin->tabs[*origin->active]);
        activate_tab(added, 0);
    }
    groups_.push_back(std::move(added));

    const Orientation axis = (direction == Direction::left || direction == Direction::right)
                                 ? Orientation::horizontal
                                 : Orientation::vertical;
    const bool after = direction == Direction::right || direction == Direction::down;

    if (path.empty()) {
        root_ = after ? splitter_of(axis, group_node(id), group_node(created))
                      : splitter_of(axis, group_node(created), group_node(id));
    } else {
        const std::size_t position = path.back();
        path.pop_back();
        LayoutNode& parent = *node_at(root_, path);
        if (parent.orientation == axis) {
            const double half = parent.ratios[position] / 2;
            parent.ratios[position] = half;
            const auto at = static_cast<std::ptrdiff_t>(position + (after ? 1 : 0));
            parent.children.insert(parent.children.begin() + at, group_node(created));
            parent.ratios.insert(parent.ratios.begin() + at, half);
        } else {
            LayoutNode& slot = parent.children[position];
            slot = after ? splitter_of(axis, group_node(id), group_node(created))
                         : splitter_of(axis, group_node(created), group_node(id));
        }
    }
    focused_ = created;
    notify(before);
    return created;
}

void EditorLayout::erase_group(GroupId id) {
    const std::vector<GroupId> order = group_ids();
    const auto position = std::ranges::find(order, id) - order.begin();
    if (focused_ == id) {
        focused_ = position > 0 ? order[static_cast<std::size_t>(position - 1)]
                                : order[static_cast<std::size_t>(position + 1)];
    }

    std::vector<std::size_t> path;
    find_path(root_, id, path);
    const std::size_t child = path.back();
    path.pop_back();
    LayoutNode& parent = *node_at(root_, path);
    const double freed = parent.ratios[child];
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child));
    parent.ratios.erase(parent.ratios.begin() + static_cast<std::ptrdiff_t>(child));
    parent.ratios[child > 0 ? child - 1 : 0] += freed;
    canonicalize(root_);

    std::erase_if(groups_, [id](const EditorGroup& g) { return g.id == id; });
}

bool EditorLayout::close_group(GroupId id) {
    if (!group(id) || groups_.size() < 2) {
        return false;
    }
    const Snapshot before = snapshot();
    erase_group(id);
    notify(before);
    return true;
}

std::size_t EditorLayout::replace_document(const core::Uri& from, const core::Uri& to) {
    if (from == to) {
        return 0;
    }
    const Snapshot before = snapshot();
    std::size_t changed = 0;
    for (const GroupId id : group_ids()) {
        EditorGroup& target = *mutable_group(id);
        const auto old_index = index_of(target, from);
        if (!old_index) {
            continue;
        }
        ++changed;
        if (index_of(target, to)) {
            remove_tab(target, *old_index);
            close_group_if_empty(id);
        } else {
            std::ranges::replace(target.recent, from, to);
            target.tabs[*old_index] = to;
        }
    }
    notify(before);
    return changed;
}

void EditorLayout::set_focus_recent_after_close(bool enabled) {
    options_.focus_recent_after_close = enabled;
}

bool EditorLayout::focus(GroupId id) {
    if (!group(id)) {
        return false;
    }
    const Snapshot before = snapshot();
    focused_ = id;
    notify(before);
    return true;
}

void EditorLayout::focus_next() {
    const std::vector<GroupId> order = group_ids();
    const auto position =
        static_cast<std::size_t>(std::ranges::find(order, focused_) - order.begin());
    focus(order[(position + 1) % order.size()]);
}

void EditorLayout::focus_previous() {
    const std::vector<GroupId> order = group_ids();
    const auto position =
        static_cast<std::size_t>(std::ranges::find(order, focused_) - order.begin());
    focus(order[(position + order.size() - 1) % order.size()]);
}

bool EditorLayout::focus_at(std::size_t index) {
    const std::vector<GroupId> order = group_ids();
    return index < order.size() && focus(order[index]);
}

std::vector<GroupId> EditorLayout::groups_by_recency() const {
    std::vector<GroupId> result;
    for (const GroupId id : recent_groups_) {
        if (group(id)) {
            result.push_back(id);
        }
    }
    for (const GroupId id : group_ids()) {
        if (std::ranges::find(result, id) == result.end()) {
            result.push_back(id);
        }
    }
    return result;
}

std::optional<GroupId> EditorLayout::neighbor(GroupId id, Direction direction) const {
    std::vector<std::pair<GroupId, Box>> boxes;
    collect_boxes(root_, Box{}, boxes);
    const auto own = std::ranges::find(boxes, id, &std::pair<GroupId, Box>::first);
    if (own == boxes.end()) {
        return std::nullopt;
    }
    const Box mine = own->second;

    // The edge that the neighbors touch: the edge of this group, or the opposite edge of the area
    // when this group is at the edge.
    double edge = 0;
    switch (direction) {
    case Direction::right:
        edge = near(mine.right, 1) ? 0 : mine.right;
        break;
    case Direction::left:
        edge = near(mine.left, 0) ? 1 : mine.left;
        break;
    case Direction::down:
        edge = near(mine.bottom, 1) ? 0 : mine.bottom;
        break;
    case Direction::up:
        edge = near(mine.top, 0) ? 1 : mine.top;
        break;
    }
    const std::vector<GroupId> recency = groups_by_recency();
    std::optional<GroupId> best;
    std::size_t best_rank = 0;
    for (const auto& [candidate, box] : boxes) {
        bool touches = false;
        switch (direction) {
        case Direction::right:
            touches = near(box.left, edge) && overlap(mine.top, mine.bottom, box.top, box.bottom);
            break;
        case Direction::left:
            touches = near(box.right, edge) && overlap(mine.top, mine.bottom, box.top, box.bottom);
            break;
        case Direction::down:
            touches = near(box.top, edge) && overlap(mine.left, mine.right, box.left, box.right);
            break;
        case Direction::up:
            touches = near(box.bottom, edge) && overlap(mine.left, mine.right, box.left, box.right);
            break;
        }
        if (!touches) {
            continue;
        }
        const auto rank =
            static_cast<std::size_t>(std::ranges::find(recency, candidate) - recency.begin());
        if (!best || rank < best_rank) {
            best = candidate;
            best_rank = rank;
        }
    }
    return best;
}

bool EditorLayout::focus_neighbor(Direction direction) {
    const std::optional<GroupId> next = neighbor(focused_, direction);
    return next && *next != focused_ && focus(*next);
}

bool EditorLayout::next_editor(bool forward) {
    const std::vector<GroupId> order = group_ids();
    const EditorGroup& current = *group(focused_);
    const std::size_t count = current.tabs.size();
    std::optional<TabLocation> target;
    if (forward && current.active && *current.active + 1 < count) {
        target = TabLocation{focused_, *current.active + 1};
    } else if (!forward && current.active && *current.active > 0) {
        target = TabLocation{focused_, *current.active - 1};
    } else {
        // The next group that has tabs, going around the groups and ending at this one.
        const auto position =
            static_cast<std::size_t>(std::ranges::find(order, focused_) - order.begin());
        for (std::size_t step = 1; step <= order.size() && !target; ++step) {
            const std::size_t at =
                forward ? (position + step) % order.size()
                        : (position + order.size() - step % order.size()) % order.size();
            const EditorGroup& candidate = *group(order[at]);
            if (!candidate.tabs.empty()) {
                target = TabLocation{order[at], forward ? 0 : candidate.tabs.size() - 1};
            }
        }
    }
    if (!target) {
        return false;
    }
    const Snapshot before = snapshot();
    activate_tab(*mutable_group(target->group), target->index);
    focused_ = target->group;
    notify(before);
    return true;
}

bool EditorLayout::next_editor_in_group(bool forward) {
    EditorGroup& current = *mutable_group(focused_);
    if (current.tabs.empty()) {
        return false;
    }
    const std::size_t count = current.tabs.size();
    const std::size_t at = current.active.value_or(0);
    const std::size_t target = forward ? (current.active ? (at + 1) % count : 0)
                                       : (current.active ? (at + count - 1) % count : count - 1);
    const Snapshot before = snapshot();
    activate_tab(current, target);
    notify(before);
    return true;
}

bool EditorLayout::navigate_recent(bool older, bool in_group) {
    std::optional<RecentStack>& stack = in_group ? recent_in_group_ : recent_all_;
    if (!stack) {
        RecentStack fresh;
        for (const GroupId id : in_group ? std::vector<GroupId>{focused_} : groups_by_recency()) {
            for (const core::Uri& uri : group(id)->recent) {
                fresh.tabs.push_back({id, uri});
            }
        }
        stack = std::move(fresh);
    }
    if (stack->tabs.empty()) {
        return false;
    }
    const std::size_t last = stack->tabs.size() - 1;
    stack->index =
        older ? std::min(stack->index + 1, last) : (stack->index > 0 ? stack->index - 1 : 0);
    const RecentTab target = stack->tabs[stack->index];

    const Snapshot before = snapshot();
    (in_group ? navigating_in_group_ : navigating_all_) = true;
    EditorGroup& owner = *mutable_group(target.group);
    activate_tab(owner, *index_of(owner, target.uri));
    focused_ = target.group;
    notify(before);
    (in_group ? navigating_in_group_ : navigating_all_) = false;
    return true;
}

namespace {

Orientation orthogonal(Orientation orientation) {
    return orientation == Orientation::horizontal ? Orientation::vertical : Orientation::horizontal;
}

std::size_t group_count(const std::vector<LayoutShape>& shapes) {
    std::size_t count = 0;
    for (const LayoutShape& shape : shapes) {
        count += shape.children.empty() ? 1 : group_count(shape.children);
    }
    return count;
}

LayoutNode build_node(const std::vector<LayoutShape>& shapes, Orientation orientation,
                      const std::vector<GroupId>& groups, std::size_t& next) {
    LayoutNode node;
    node.orientation = orientation;
    for (const LayoutShape& shape : shapes) {
        node.children.push_back(
            shape.children.empty()
                ? group_node(groups[next++])
                : build_node(shape.children, orthogonal(orientation), groups, next));
        node.ratios.push_back(1.0 / static_cast<double>(shapes.size()));
    }
    return node;
}

void flip_node(LayoutNode& node) {
    if (node.group) {
        return;
    }
    node.orientation = orthogonal(node.orientation);
    for (LayoutNode& child : node.children) {
        flip_node(child);
    }
}

} // namespace

void EditorLayout::apply_layout(Orientation orientation, const std::vector<LayoutShape>& children) {
    const std::size_t wanted = std::max<std::size_t>(group_count(children), 1);
    const Snapshot before = snapshot();
    const std::vector<GroupId> order = group_ids();

    std::vector<GroupId> groups(
        order.begin(), order.begin() + static_cast<std::ptrdiff_t>(std::min(wanted, order.size())));
    if (order.size() > wanted) {
        EditorGroup& target = *mutable_group(order[wanted - 1]);
        std::optional<core::Uri> shown;
        for (std::size_t i = wanted; i < order.size(); ++i) {
            const EditorGroup& source = *group(order[i]);
            for (const core::Uri& uri : source.tabs) {
                if (!index_of(target, uri)) {
                    target.tabs.push_back(uri);
                    target.recent.push_back(uri);
                }
            }
            if (order[i] == focused_ && source.active) {
                shown = source.tabs[*source.active];
            }
        }
        const bool focused_goes =
            std::ranges::find(order.begin() + static_cast<std::ptrdiff_t>(wanted), order.end(),
                              focused_) != order.end();
        if (focused_goes) {
            focused_ = target.id;
            if (shown) {
                activate_tab(target, *index_of(target, *shown));
            }
        }
        if (!target.active && !target.tabs.empty()) {
            activate_tab(target, 0);
        }
        std::erase_if(groups_, [&](const EditorGroup& g) {
            return std::ranges::find(order.begin() + static_cast<std::ptrdiff_t>(wanted),
                                     order.end(), g.id) != order.end();
        });
    }
    while (groups.size() < wanted) {
        const GroupId created{next_id_++};
        groups_.push_back({.id = created, .tabs = {}, .active = std::nullopt, .recent = {}});
        groups.push_back(created);
    }

    std::size_t next = 0;
    root_ = build_node(children, orientation, groups, next);
    if (root_.children.empty()) {
        root_ = group_node(groups.front());
    }
    canonicalize(root_);
    notify(before);
}

bool EditorLayout::flip() {
    if (groups_.size() < 2) {
        return false;
    }
    const Snapshot before = snapshot();
    flip_node(root_);
    notify(before);
    return true;
}

bool EditorLayout::set_ratios(const std::vector<std::size_t>& path, std::vector<double> ratios) {
    LayoutNode* node = node_at(root_, path);
    if (!node || node->group || ratios.size() != node->children.size()) {
        return false;
    }
    double sum = 0;
    for (const double ratio : ratios) {
        if (!(ratio > 0) || !std::isfinite(ratio)) {
            return false;
        }
        sum += ratio;
    }
    const Snapshot before = snapshot();
    for (double& ratio : ratios) {
        ratio /= sum;
    }
    node->ratios = std::move(ratios);
    notify(before);
    return true;
}

namespace {

std::optional<std::string> check_node(const LayoutNode& node, const LayoutNode* parent) {
    if (node.group) {
        if (!node.children.empty() || !node.ratios.empty()) {
            return "a group node has children";
        }
        return std::nullopt;
    }
    if (node.children.size() < 2) {
        return "a splitter has fewer than two children";
    }
    if (node.ratios.size() != node.children.size()) {
        return "a splitter has a different number of ratios than children";
    }
    if (parent && !parent->group && parent->orientation == node.orientation) {
        return "a splitter has the orientation of its parent";
    }
    double sum = 0;
    for (const double ratio : node.ratios) {
        if (!(ratio > 0) || !std::isfinite(ratio)) {
            return "a ratio is not positive";
        }
        sum += ratio;
    }
    if (std::abs(sum - 1) > ratio_tolerance) {
        return "the ratios of a splitter do not add up to one";
    }
    for (const LayoutNode& child : node.children) {
        if (auto problem = check_node(child, &node)) {
            return problem;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> EditorLayout::find_inconsistency() const {
    if (auto problem = check_node(root_, nullptr)) {
        return problem;
    }
    std::vector<GroupId> in_tree = group_ids();
    std::vector<GroupId> stored;
    for (const EditorGroup& g : groups_) {
        stored.push_back(g.id);
    }
    std::ranges::sort(in_tree);
    std::ranges::sort(stored);
    if (std::ranges::adjacent_find(in_tree) != in_tree.end() ||
        std::ranges::adjacent_find(stored) != stored.end()) {
        return "a group is repeated";
    }
    if (in_tree != stored) {
        return "the groups of the tree and the groups that hold tabs differ";
    }
    for (const EditorGroup& g : groups_) {
        if (static_cast<std::uint32_t>(g.id) >= next_id_) {
            return "a group id is not below the next id";
        }
        if (g.active.has_value() != !g.tabs.empty() || (g.active && *g.active >= g.tabs.size())) {
            return "the active tab of a group is wrong";
        }
        auto sorted_tabs = g.tabs;
        auto sorted_recent = g.recent;
        std::ranges::sort(sorted_tabs);
        std::ranges::sort(sorted_recent);
        if (sorted_tabs != sorted_recent || (g.active && g.recent.front() != g.tabs[*g.active])) {
            return "the recent tabs of a group are not its tabs with the active one first";
        }
        for (std::size_t i = 0; i < g.tabs.size(); ++i) {
            if (std::ranges::find(g.tabs.begin() + static_cast<std::ptrdiff_t>(i) + 1, g.tabs.end(),
                                  g.tabs[i]) != g.tabs.end()) {
                return "a document is twice in a group";
            }
        }
    }
    if (!group(focused_)) {
        return "the focused group does not exist";
    }
    return std::nullopt;
}

namespace {

nlohmann::json uri_to_json(const core::Uri& uri) {
    nlohmann::json json = nlohmann::json::object();
    json["scheme"] = uri.scheme;
    for (const auto& [key, value] :
         {std::pair<const char*, const std::string&>{"authority", uri.authority},
          {"path", uri.path},
          {"query", uri.query},
          {"fragment", uri.fragment}}) {
        if (!value.empty()) {
            json[key] = value;
        }
    }
    return json;
}

} // namespace

EditorLayout EditorLayout::retained(const std::function<bool(const core::Uri&)>& keep) const {
    EditorLayout copy = *this;
    copy.observers_.clear();
    copy.recent_all_.reset();
    copy.recent_in_group_.reset();
    for (const GroupId id : group_ids()) {
        EditorGroup& target = *copy.mutable_group(id);
        const bool had_tabs = !target.tabs.empty();
        for (std::size_t index = target.tabs.size(); index > 0; --index) {
            if (!keep(target.tabs[index - 1])) {
                copy.remove_tab(target, index - 1);
            }
        }
        if (had_tabs && target.tabs.empty() && copy.groups_.size() > 1) {
            copy.erase_group(id);
        }
    }
    std::erase_if(copy.recent_groups_, [&](GroupId id) { return !copy.group(id); });
    return copy;
}

void EditorLayout::restore(const EditorLayout& other) {
    const Snapshot before = snapshot();
    std::map<GroupId, GroupId> renamed;
    for (const GroupId id : other.group_ids()) {
        renamed.emplace(id, GroupId{next_id_++});
    }
    const auto rename = [&](const auto& self, LayoutNode& node) -> void {
        if (node.group) {
            node.group = renamed.at(*node.group);
        }
        for (LayoutNode& child : node.children) {
            self(self, child);
        }
    };
    root_ = other.root_;
    rename(rename, root_);
    groups_ = other.groups_;
    for (EditorGroup& group : groups_) {
        group.id = renamed.at(group.id);
    }
    focused_ = renamed.at(other.focused_);
    recent_groups_.clear();
    for (const GroupId id : other.recent_groups_) {
        recent_groups_.push_back(renamed.at(id));
    }
    recent_all_.reset();
    recent_in_group_.reset();
    notify(before);
}

nlohmann::json EditorLayout::to_json() const {
    const auto node_json = [this](const auto& self, const LayoutNode& node) -> nlohmann::json {
        nlohmann::json json = nlohmann::json::object();
        if (node.group) {
            const EditorGroup& g = *group(*node.group);
            json["group"] = static_cast<std::uint32_t>(g.id);
            json["tabs"] = nlohmann::json::array();
            for (const core::Uri& uri : g.tabs) {
                json["tabs"].push_back(uri_to_json(uri));
            }
            json["active"] = g.active ? nlohmann::json(*g.active) : nlohmann::json(nullptr);
            json["recent"] = nlohmann::json::array();
            for (const core::Uri& uri : g.recent) {
                json["recent"].push_back(*index_of(g, uri));
            }
            return json;
        }
        json["orientation"] =
            node.orientation == Orientation::horizontal ? "horizontal" : "vertical";
        json["ratios"] = node.ratios;
        json["children"] = nlohmann::json::array();
        for (const LayoutNode& child : node.children) {
            json["children"].push_back(self(self, child));
        }
        return json;
    };
    nlohmann::json json = nlohmann::json::object();
    json["version"] = 1;
    json["focused"] = static_cast<std::uint32_t>(focused_);
    json["next_group_id"] = next_id_;
    json["root"] = node_json(node_json, root_);
    return json;
}

namespace {

struct ParseFailure {
    std::string message;
};

std::string string_field(const nlohmann::json& json, const char* key, bool required) {
    const auto found = json.find(key);
    if (found == json.end()) {
        if (required) {
            throw ParseFailure{std::string("missing \"") + key + "\""};
        }
        return {};
    }
    if (!found->is_string()) {
        throw ParseFailure{std::string("\"") + key + "\" is not a string"};
    }
    return found->get<std::string>();
}

std::uint32_t id_field(const nlohmann::json& json, const char* key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0 ||
        found->get<std::int64_t>() > std::numeric_limits<std::uint32_t>::max()) {
        throw ParseFailure{std::string("\"") + key + "\" is not an identifier"};
    }
    return found->get<std::uint32_t>();
}

} // namespace

std::expected<EditorLayout, Error> EditorLayout::from_json(const nlohmann::json& json,
                                                           EditorLayoutOptions options) {
    const auto fail = [](const std::string& why) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid editor layout: " + why});
    };
    try {
        if (!json.is_object() || id_field(json, "version") != 1) {
            return fail("not a layout of a known version");
        }
        EditorLayout layout(options);
        layout.groups_.clear();
        layout.next_id_ = id_field(json, "next_group_id");
        layout.focused_ = GroupId{id_field(json, "focused")};
        layout.recent_groups_ = {layout.focused_};

        const auto read_node = [&](const auto& self, const nlohmann::json& node) -> LayoutNode {
            if (!node.is_object()) {
                throw ParseFailure{"a node is not an object"};
            }
            LayoutNode result;
            if (node.contains("group")) {
                EditorGroup g;
                g.id = GroupId{id_field(node, "group")};
                const auto tabs = node.find("tabs");
                if (tabs == node.end() || !tabs->is_array()) {
                    throw ParseFailure{"\"tabs\" is not an array"};
                }
                for (const nlohmann::json& tab : *tabs) {
                    if (!tab.is_object()) {
                        throw ParseFailure{"a tab is not an object"};
                    }
                    g.tabs.push_back({.scheme = string_field(tab, "scheme", true),
                                      .authority = string_field(tab, "authority", false),
                                      .path = string_field(tab, "path", false),
                                      .query = string_field(tab, "query", false),
                                      .fragment = string_field(tab, "fragment", false)});
                }
                const auto active = node.find("active");
                if (active != node.end() && !active->is_null()) {
                    if (!active->is_number_integer() || active->get<std::int64_t>() < 0) {
                        throw ParseFailure{"\"active\" is not a number"};
                    }
                    g.active = active->get<std::size_t>();
                }
                if (const auto recent = node.find("recent"); recent != node.end()) {
                    if (!recent->is_array()) {
                        throw ParseFailure{"\"recent\" is not an array"};
                    }
                    for (const nlohmann::json& position : *recent) {
                        if (!position.is_number_integer() || position.get<std::int64_t>() < 0 ||
                            position.get<std::uint64_t>() >= g.tabs.size()) {
                            throw ParseFailure{"a recent tab is not a position of a tab"};
                        }
                        g.recent.push_back(g.tabs[position.get<std::size_t>()]);
                    }
                } else {
                    g.recent = g.tabs;
                    if (g.active && *g.active < g.tabs.size()) {
                        std::rotate(g.recent.begin(),
                                    g.recent.begin() + static_cast<std::ptrdiff_t>(*g.active),
                                    g.recent.begin() + static_cast<std::ptrdiff_t>(*g.active) + 1);
                    }
                }
                result.group = g.id;
                layout.groups_.push_back(std::move(g));
                return result;
            }
            const std::string orientation = string_field(node, "orientation", true);
            if (orientation != "horizontal" && orientation != "vertical") {
                throw ParseFailure{"unknown orientation"};
            }
            result.orientation =
                orientation == "horizontal" ? Orientation::horizontal : Orientation::vertical;
            const auto children = node.find("children");
            const auto ratios = node.find("ratios");
            if (children == node.end() || !children->is_array() || ratios == node.end() ||
                !ratios->is_array()) {
                throw ParseFailure{"a splitter needs \"children\" and \"ratios\" arrays"};
            }
            for (const nlohmann::json& ratio : *ratios) {
                if (!ratio.is_number()) {
                    throw ParseFailure{"a ratio is not a number"};
                }
                result.ratios.push_back(ratio.get<double>());
            }
            for (const nlohmann::json& child : *children) {
                result.children.push_back(self(self, child));
            }
            return result;
        };
        const auto root = json.find("root");
        if (root == json.end()) {
            return fail("missing \"root\"");
        }
        layout.root_ = read_node(read_node, *root);
        if (const auto problem = layout.find_inconsistency()) {
            return fail(*problem);
        }
        return layout;
    } catch (const ParseFailure& failure) {
        return fail(failure.message);
    } catch (const nlohmann::json::exception& exception) {
        return fail(exception.what());
    }
}

} // namespace anvil::workbench
