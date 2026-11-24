#pragma once

#include "anvil/core/text.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/workbench/editor_layout.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace anvil::workbench {

// The primary selection of an editor as lines and columns, which survive edits elsewhere in the
// text better than offsets do.
struct NavigationSelection {
    core::Position anchor;
    core::Position active;

    // The line the selection starts on: the first of its two ends.
    std::size_t line() const {
        return anchor.line < active.line ? anchor.line : active.line;
    }

    bool operator==(const NavigationSelection&) const = default;
};

// Why the selection of an editor changed. A `navigation` (go to a line, a definition) is a place
// the user chose to go to, and gets an entry even when it is near the last one.
enum class SelectionReason { user, edit, navigation };

// A place in the history: an editor of a group and where its selection was.
struct NavigationEntry {
    GroupId group{};
    core::Uri uri;
    NavigationSelection selection;

    bool operator==(const NavigationEntry&) const = default;
};

// A list of places and the one the user is at, with the rules of VS Code's editor navigation
// stack. A new place is added after the current one, which drops the places that were ahead of it,
// unless it is in the same editor of the same group within `similar_line_distance` lines of the
// state it follows, in which case it replaces the current place. The list is bounded, and a place
// that is the same as its neighbor after removals is dropped.
class NavigationStack {
public:
    static constexpr std::size_t max_entries = 50;
    // Selections whose first lines differ by fewer lines are the same area of the text.
    static constexpr std::size_t similar_line_distance = 10;

    // Records where the user is. `reason` is empty when the editor was activated and nothing
    // changed in it.
    void notify(GroupId group, const core::Uri& uri, const NavigationSelection& selection,
                std::optional<SelectionReason> reason);
    // Takes `group`, `uri` and `selection` as the place the user is at without adding or changing
    // an entry: the user got there by walking the list.
    void settle(GroupId group, const core::Uri& uri, const NavigationSelection& selection);

    bool can_go_back() const;
    bool can_go_forward() const;
    bool can_go_last() const;
    // Move to another place and return it.
    std::optional<NavigationEntry> go_back();
    std::optional<NavigationEntry> go_forward();
    std::optional<NavigationEntry> go_last();

    // The entry the user is at, or null when the list is empty.
    const NavigationEntry* current() const;
    const std::vector<NavigationEntry>& entries() const {
        return entries_;
    }

    void remove_uri(const core::Uri& uri);
    void remove_group(GroupId group);
    void remove_if(const std::function<bool(const NavigationEntry&)>& matches);
    // The editor of `uri` moved from one group to another.
    void move_editor(GroupId from, GroupId to, const core::Uri& uri);
    void replace_uri(const core::Uri& from, const core::Uri& to);
    void clear();

private:
    struct State {
        GroupId group{};
        core::Uri uri;
        NavigationSelection selection;
        std::optional<SelectionReason> reason;
    };

    bool justifies_new_entry(const State& candidate) const;
    void add_or_replace(NavigationEntry entry, bool force_replace);

    std::vector<NavigationEntry> entries_;
    std::size_t index_ = 0;
    std::optional<State> state_;
};

// The places of the editors of a window. `selections` has every place the user has been, in the
// order of the visits, and Back and Forward walk it; `edits` has the places where the text changed,
// and Last Edit Location goes to the newest.
class NavigationHistory {
public:
    // An editor became the one the user works in.
    void editor_activated(GroupId group, const core::Uri& uri,
                          const NavigationSelection& selection);
    // The selection of the active editor changed.
    void selection_changed(GroupId group, const core::Uri& uri,
                           const NavigationSelection& selection, SelectionReason reason);

    bool can_go_back() const {
        return selections_.can_go_back();
    }
    bool can_go_forward() const {
        return selections_.can_go_forward();
    }
    bool can_go_to_last_edit() const {
        return edits_.can_go_last();
    }
    // The place to show for Back, Forward and Last Edit Location. The caller shows it and then
    // says where the user ended up with `arrived` or `arrived_at_edit`, so that showing it does not
    // make a place of its own.
    std::optional<NavigationEntry> go_back();
    std::optional<NavigationEntry> go_forward();
    std::optional<NavigationEntry> go_to_last_edit();
    void arrived(GroupId group, const core::Uri& uri, const NavigationSelection& selection);
    // The place of the last edit is in the list of edits and, as a place the user went to, in the
    // other list too.
    void arrived_at_edit(GroupId group, const core::Uri& uri, const NavigationSelection& selection);

    const NavigationStack& selections() const {
        return selections_;
    }
    const NavigationStack& edits() const {
        return edits_;
    }

    void remove_uri(const core::Uri& uri);
    void remove_group(GroupId group);
    void remove_if(const std::function<bool(const NavigationEntry&)>& matches);
    void move_editor(GroupId from, GroupId to, const core::Uri& uri);
    void replace_uri(const core::Uri& from, const core::Uri& to);
    void clear();

private:
    NavigationStack selections_;
    NavigationStack edits_;
};

} // namespace anvil::workbench
