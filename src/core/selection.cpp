#include "anvil/core/selection.hpp"

#include <numeric>

namespace anvil::core {

SelectionSet SelectionSet::from(std::vector<Selection> selections, std::size_t primary) {
    SelectionSet result;
    if (selections.empty()) {
        return result;
    }
    primary = std::min(primary, selections.size() - 1);

    std::vector<std::size_t> order(selections.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return selections[a].start() < selections[b].start();
    });

    result.selections_.clear();
    bool group_has_primary = false;
    Selection group;
    bool have_group = false;
    for (const std::size_t index : order) {
        const Selection& next = selections[index];
        const bool touches =
            have_group && (next.start() < group.end() ||
                           (next.start() == group.end() && (group.empty() || next.empty())));
        if (touches) {
            // The merged selection keeps the direction of the primary selection when it is part
            // of the merge, and otherwise that of the earlier selection.
            const bool reversed = index == primary ? next.reversed() : group.reversed();
            const std::size_t start = std::min(group.start(), next.start());
            const std::size_t end = std::max(group.end(), next.end());
            group = reversed ? Selection{end, start} : Selection{start, end};
            group_has_primary = group_has_primary || index == primary;
            continue;
        }
        if (have_group) {
            if (group_has_primary) {
                result.primary_ = result.selections_.size();
            }
            result.selections_.push_back(group);
        }
        group = next;
        group_has_primary = index == primary;
        have_group = true;
    }
    if (group_has_primary) {
        result.primary_ = result.selections_.size();
    }
    result.selections_.push_back(group);
    return result;
}

} // namespace anvil::core
