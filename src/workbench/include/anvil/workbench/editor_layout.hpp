#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/uri.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace anvil::workbench {

enum class GroupId : std::uint32_t {};

// The direction a splitter lays its children out in: side by side, or one above the other.
enum class Orientation { horizontal, vertical };

enum class Direction { left, right, up, down };

// What a new group holds when it is split off another.
enum class SplitContent {
    empty,
    // The new group shows the active tab of the group it was split from.
    copy_active,
};

// A group of tabs: the documents it shows, in order, and which of them is the active one.
struct EditorGroup {
    GroupId id{};
    std::vector<core::Uri> tabs;
    // The position of the active tab; empty exactly when the group has no tabs.
    std::optional<std::size_t> active;

    bool operator==(const EditorGroup&) const = default;
};

// A node of the tree that arranges the groups: a group, or a splitter with two or more children
// that are laid out in one direction, each taking the share of the splitter that its ratio says.
// The ratios of a splitter are positive and add up to one. A child of a splitter is a group or a
// splitter of the other orientation, so that every splitter is a real division of the space.
struct LayoutNode {
    // Set for a group, empty for a splitter.
    std::optional<GroupId> group;
    Orientation orientation = Orientation::horizontal;
    std::vector<LayoutNode> children;
    std::vector<double> ratios;

    bool is_group() const {
        return group.has_value();
    }
    bool operator==(const LayoutNode&) const = default;
};

struct TabLocation {
    GroupId group{};
    std::size_t index = 0;

    bool operator==(const TabLocation&) const = default;
};

struct EditorLayoutOptions {
    // As VS Code's `workbench.editor.closeEmptyGroups`: a group that loses its last tab to closing
    // or to a move is closed, unless it is the only group.
    bool close_empty_groups = true;
};

// The editor area as plain data: the groups of tabs, which of them has the focus, and the tree
// that arranges them. It holds no widgets, so a window can be built from it and kept in step with
// it, and so that it can be tested and saved without a GUI.
//
// A tab is a document's URI, and a document is at most once in a group; a document can be in more
// than one group. The window starts with one empty group that has the focus. Every operation
// leaves the model consistent, and reports what changed to the observers afterwards, not while it
// is still half done.
class EditorLayout {
public:
    // What changed, as far as groups that exist both before and after an operation go. A group that
    // an operation creates or removes is part of `structure_changed`; a window reads the state of a
    // new group when it builds it.
    class Observer {
    public:
        virtual ~Observer() = default;
        // The tree changed shape: a group was added or removed, or the groups were rearranged.
        virtual void structure_changed() {}
        // Only the ratios of splitters changed.
        virtual void ratios_changed() {}
        // The tabs of the group changed: one was added, removed or moved.
        virtual void tabs_changed(GroupId) {}
        // A different tab is the active one of the group, or the group got its first tab or lost
        // its last.
        virtual void active_tab_changed(GroupId) {}
        virtual void focus_changed(GroupId) {}
    };

    explicit EditorLayout(EditorLayoutOptions options = {});

    void add_observer(Observer* observer);
    void remove_observer(Observer* observer);

    const LayoutNode& root() const {
        return root_;
    }
    // The groups in the order they are shown: left to right and top to bottom.
    std::vector<GroupId> group_ids() const;
    const EditorGroup* group(GroupId id) const;
    GroupId focused() const {
        return focused_;
    }
    // Where the document is open: in each group that has it, in the order of the groups.
    std::vector<TabLocation> locations(const core::Uri& uri) const;

    // Opens the document in the group and makes it the active tab, or only activates it when the
    // group has it. A new tab goes to the right of the active tab, or at the end when there is
    // none, as in VS Code. Returns the position of the tab, or nothing for a group that does not
    // exist.
    std::optional<std::size_t> open(GroupId group, const core::Uri& uri);
    // Shows the document where it is: the focused group when it has it, else the first group that
    // does, which then gets the focus; a document that is open nowhere opens in the focused group.
    TabLocation show(const core::Uri& uri);
    // Makes the tab the active one of its group. Returns false when there is no such tab.
    bool activate(GroupId group, std::size_t index);
    // Closes the tab. When it was the active tab, the one to its right becomes active, or the one
    // to its left when it was the last. Returns false when there is no such tab.
    bool close_tab(GroupId group, std::size_t index);
    // Moves a tab within its group, to `to_index` in the order that results, or to another group,
    // at `to_index` there; an index past the end means the end. A tab moved to another group
    // becomes the active tab there and the group gets the focus; when that group has the document
    // already, the tab is closed in the source and the existing one activated. The source group
    // activates a neighbor as when a tab closes. Returns false for a tab or group that does not
    // exist.
    bool move_tab(GroupId from, std::size_t from_index, GroupId to, std::size_t to_index);

    // Splits a group in a direction, which adds a new group on that side that takes half of the
    // space of the group, and gives the new group the focus. Returns the new group, or nothing for
    // a group that does not exist.
    std::optional<GroupId> split(GroupId group, Direction direction,
                                 SplitContent content = SplitContent::empty);
    // Removes the group with its tabs, and gives its space to the sibling before it in the tree, or
    // after it when it was the first. A splitter left with one child is replaced by it. When the
    // closed group had the focus, it moves to the group before it in the order they are shown, or
    // after it when it was the first. The only group cannot be closed.
    bool close_group(GroupId group);

    // Makes every tab of the document `from` show `to` in its place, as when a document is saved
    // under another name. A group that has `to` already closes its tab of `from` instead, as when
    // a tab closes. Returns the number of groups that changed.
    std::size_t replace_document(const core::Uri& from, const core::Uri& to);

    bool focus(GroupId group);
    // Moves the focus to the next or previous group in the order they are shown, wrapping around.
    void focus_next();
    void focus_previous();

    // Sets the ratios of the splitter that is reached from the root by the given child positions,
    // scaled to add up to one. Returns false for a path that is not a splitter, a wrong number of
    // ratios or a ratio that is not positive.
    bool set_ratios(const std::vector<std::size_t>& path, std::vector<double> ratios);

    // A description of the first way the model breaks its rules, or nothing when it holds them:
    // the tree has splitters with fewer than two children or of the orientation of their parent,
    // ratios that are wrong, groups that are missing, repeated or not in the tree, a document twice
    // in a group, an active tab out of range, or a focused group that does not exist.
    std::optional<std::string> find_inconsistency() const;

    nlohmann::json to_json() const;
    static std::expected<EditorLayout, Error> from_json(const nlohmann::json& json,
                                                        EditorLayoutOptions options = {});

private:
    struct Snapshot;

    Snapshot snapshot() const;
    void notify(const Snapshot& before) const;
    EditorGroup* mutable_group(GroupId id);
    static void remove_tab(EditorGroup& group, std::size_t index);
    std::optional<std::size_t> open_in(EditorGroup& group, const core::Uri& uri);
    void erase_group(GroupId id);
    void close_group_if_empty(GroupId id);

    EditorLayoutOptions options_;
    LayoutNode root_;
    std::vector<EditorGroup> groups_;
    GroupId focused_{};
    std::uint32_t next_id_ = 1;
    std::vector<Observer*> observers_;
};

} // namespace anvil::workbench
