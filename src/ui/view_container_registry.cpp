#include "anvil/ui/view_container_registry.hpp"

#include <algorithm>

namespace anvil::ui {

void ViewContainerRegistry::add(ViewContainer container) {
    if (auto existing = std::ranges::find(containers_, container.id, &ViewContainer::id);
        existing != containers_.end()) {
        *existing = std::move(container);
    } else {
        containers_.push_back(std::move(container));
    }
    emit changed();
}

bool ViewContainerRegistry::remove(std::string_view id) {
    const auto found = std::ranges::find(containers_, id, &ViewContainer::id);
    if (found == containers_.end()) {
        return false;
    }
    containers_.erase(found);
    emit changed();
    return true;
}

const ViewContainer* ViewContainerRegistry::find(std::string_view id) const {
    const auto found = std::ranges::find(containers_, id, &ViewContainer::id);
    return found == containers_.end() ? nullptr : &*found;
}

std::vector<const ViewContainer*>
ViewContainerRegistry::containers(ViewContainerLocation location) const {
    std::vector<const ViewContainer*> result;
    for (const ViewContainer& container : containers_) {
        if (container.location == location) {
            result.push_back(&container);
        }
    }
    std::ranges::stable_sort(result, {}, &ViewContainer::order);
    return result;
}

} // namespace anvil::ui
