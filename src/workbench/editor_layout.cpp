#include "anvil/workbench/editor_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

std::optional<std::size_t> index_of(const EditorGroup& group, const core::Uri& uri) {
    const auto found = std::ranges::find(group.tabs, uri);
    if (found == group.tabs.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - group.tabs.begin());
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
    groups_.push_back({.id = first, .tabs = {}, .active = std::nullopt});
    focused_ = first;
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

void EditorLayout::notify(const Snapshot& before) const {
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
        target.active = *existing;
        return existing;
    }
    const std::size_t position = target.active ? *target.active + 1 : target.tabs.size();
    target.tabs.insert(target.tabs.begin() + static_cast<std::ptrdiff_t>(position), uri);
    target.active = position;
    return position;
}

void EditorLayout::remove_tab(EditorGroup& target, std::size_t index) {
    target.tabs.erase(target.tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (target.tabs.empty()) {
        target.active = std::nullopt;
    } else if (index == *target.active) {
        target.active = std::min(index, target.tabs.size() - 1);
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
    mutable_group(location.group)->active = location.index;
    notify(before);
    return location;
}

bool EditorLayout::activate(GroupId id, std::size_t index) {
    EditorGroup* target = mutable_group(id);
    if (!target || index >= target->tabs.size()) {
        return false;
    }
    const Snapshot before = snapshot();
    target->active = index;
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
            destination->active = existing;
        } else {
            const std::size_t position = std::min(to_index, destination->tabs.size());
            destination->tabs.insert(
                destination->tabs.begin() + static_cast<std::ptrdiff_t>(position), moved);
            destination->active = position;
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
    EditorGroup added{.id = created, .tabs = {}, .active = std::nullopt};
    if (content == SplitContent::copy_active && origin->active) {
        added.tabs.push_back(origin->tabs[*origin->active]);
        added.active = 0;
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
