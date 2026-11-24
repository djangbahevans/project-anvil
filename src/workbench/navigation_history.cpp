#include "anvil/workbench/navigation_history.hpp"

#include <algorithm>

namespace anvil::workbench {
namespace {

std::size_t distance(std::size_t a, std::size_t b) {
    return a < b ? b - a : a - b;
}

bool same_place(const NavigationEntry& entry, GroupId group, const core::Uri& uri,
                const NavigationSelection& selection) {
    return entry.group == group && entry.uri == uri && entry.selection.line() == selection.line();
}

} // namespace

bool NavigationStack::justifies_new_entry(const State& candidate) const {
    const State& last = *state_;
    if (last.group != candidate.group || last.uri != candidate.uri) {
        return true;
    }
    const std::size_t lines = distance(last.selection.line(), candidate.selection.line());
    if (lines == 0) {
        return false;
    }
    if (lines < similar_line_distance) {
        return candidate.reason == SelectionReason::navigation;
    }
    return true;
}

void NavigationStack::notify(GroupId group, const core::Uri& uri,
                             const NavigationSelection& selection,
                             std::optional<SelectionReason> reason) {
    const State candidate{group, uri, selection, reason};
    if (!state_ || justifies_new_entry(candidate)) {
        add_or_replace({group, uri, selection}, false);
    } else {
        add_or_replace({group, uri, selection}, true);
    }
    state_ = candidate;
}

void NavigationStack::settle(GroupId group, const core::Uri& uri,
                             const NavigationSelection& selection) {
    state_ = State{group, uri, selection, std::nullopt};
}

void NavigationStack::add_or_replace(NavigationEntry entry, bool force_replace) {
    const NavigationEntry* last = current();
    const bool replace =
        last && (force_replace || same_place(*last, entry.group, entry.uri, entry.selection));
    if (replace) {
        entries_[index_] = std::move(entry);
        return;
    }
    if (!entries_.empty()) {
        entries_.resize(index_ + 1);
    }
    entries_.push_back(std::move(entry));
    if (entries_.size() > max_entries) {
        entries_.erase(entries_.begin());
    }
    index_ = entries_.size() - 1;
}

const NavigationEntry* NavigationStack::current() const {
    return entries_.empty() ? nullptr : &entries_[index_];
}

bool NavigationStack::can_go_back() const {
    return index_ > 0;
}

bool NavigationStack::can_go_forward() const {
    return !entries_.empty() && index_ + 1 < entries_.size();
}

bool NavigationStack::can_go_last() const {
    return !entries_.empty();
}

std::optional<NavigationEntry> NavigationStack::go_back() {
    if (!can_go_back()) {
        return std::nullopt;
    }
    return entries_[--index_];
}

std::optional<NavigationEntry> NavigationStack::go_forward() {
    if (!can_go_forward()) {
        return std::nullopt;
    }
    return entries_[++index_];
}

std::optional<NavigationEntry> NavigationStack::go_last() {
    if (!can_go_last()) {
        return std::nullopt;
    }
    index_ = entries_.size() - 1;
    return entries_[index_];
}

void NavigationStack::remove_if(const std::function<bool(const NavigationEntry&)>& matches) {
    const std::size_t before = entries_.size();
    std::erase_if(entries_, matches);
    if (entries_.size() == before) {
        return;
    }
    std::vector<NavigationEntry> flattened;
    for (NavigationEntry& entry : entries_) {
        if (!flattened.empty() &&
            same_place(flattened.back(), entry.group, entry.uri, entry.selection)) {
            continue;
        }
        flattened.push_back(std::move(entry));
    }
    entries_ = std::move(flattened);
    index_ = entries_.empty() ? 0 : entries_.size() - 1;
}

void NavigationStack::remove_uri(const core::Uri& uri) {
    remove_if([&](const NavigationEntry& entry) { return entry.uri == uri; });
}

void NavigationStack::remove_group(GroupId group) {
    remove_if([&](const NavigationEntry& entry) { return entry.group == group; });
}

void NavigationStack::move_editor(GroupId from, GroupId to, const core::Uri& uri) {
    for (NavigationEntry& entry : entries_) {
        if (entry.group == from && entry.uri == uri) {
            entry.group = to;
        }
    }
    if (state_ && state_->group == from && state_->uri == uri) {
        state_->group = to;
    }
}

void NavigationStack::replace_uri(const core::Uri& from, const core::Uri& to) {
    for (NavigationEntry& entry : entries_) {
        if (entry.uri == from) {
            entry.uri = to;
        }
    }
    if (state_ && state_->uri == from) {
        state_->uri = to;
    }
}

void NavigationStack::clear() {
    entries_.clear();
    index_ = 0;
    state_.reset();
}

void NavigationHistory::editor_activated(GroupId group, const core::Uri& uri,
                                         const NavigationSelection& selection) {
    selections_.notify(group, uri, selection, std::nullopt);
}

void NavigationHistory::selection_changed(GroupId group, const core::Uri& uri,
                                          const NavigationSelection& selection,
                                          SelectionReason reason) {
    selections_.notify(group, uri, selection, reason);
    if (reason == SelectionReason::edit) {
        edits_.notify(group, uri, selection, reason);
    }
}

std::optional<NavigationEntry> NavigationHistory::go_back() {
    return selections_.go_back();
}

std::optional<NavigationEntry> NavigationHistory::go_forward() {
    return selections_.go_forward();
}

std::optional<NavigationEntry> NavigationHistory::go_to_last_edit() {
    return edits_.go_last();
}

void NavigationHistory::arrived(GroupId group, const core::Uri& uri,
                                const NavigationSelection& selection) {
    selections_.settle(group, uri, selection);
}

void NavigationHistory::arrived_at_edit(GroupId group, const core::Uri& uri,
                                        const NavigationSelection& selection) {
    edits_.settle(group, uri, selection);
    selections_.notify(group, uri, selection, SelectionReason::navigation);
}

void NavigationHistory::remove_uri(const core::Uri& uri) {
    selections_.remove_uri(uri);
    edits_.remove_uri(uri);
}

void NavigationHistory::remove_group(GroupId group) {
    selections_.remove_group(group);
    edits_.remove_group(group);
}

void NavigationHistory::remove_if(const std::function<bool(const NavigationEntry&)>& matches) {
    selections_.remove_if(matches);
    edits_.remove_if(matches);
}

void NavigationHistory::move_editor(GroupId from, GroupId to, const core::Uri& uri) {
    selections_.move_editor(from, to, uri);
    edits_.move_editor(from, to, uri);
}

void NavigationHistory::replace_uri(const core::Uri& from, const core::Uri& to) {
    selections_.replace_uri(from, to);
    edits_.replace_uri(from, to);
}

void NavigationHistory::clear() {
    selections_.clear();
    edits_.clear();
}

} // namespace anvil::workbench
