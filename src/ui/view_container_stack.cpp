#include "anvil/ui/view_container_stack.hpp"

#include <vector>

namespace anvil::ui {

ViewContainerStack::ViewContainerStack(const ViewContainerRegistry& registry,
                                       ViewContainerLocation location, QWidget* parent)
    : registry_(registry), location_(location), stack_(new QStackedWidget(parent)) {}

const ViewContainer* ViewContainerStack::show(std::string_view id) {
    const ViewContainer* container = registry_.find(id);
    if (!container || container->location != location_) {
        return nullptr;
    }
    QPointer<QWidget>& slot = widgets_[container->id];
    if (!slot) {
        QWidget* widget = container->create ? container->create() : nullptr;
        slot = widget ? widget : new QWidget;
        stack_->addWidget(slot);
    }
    stack_->setCurrentWidget(slot);
    return container;
}

QWidget* ViewContainerStack::container_widget(std::string_view id) const {
    const auto found = widgets_.find(std::string(id));
    return found == widgets_.end() ? nullptr : found->second.data();
}

void ViewContainerStack::prune() {
    std::vector<std::string> gone;
    for (const auto& [id, widget] : widgets_) {
        const ViewContainer* container = registry_.find(id);
        if (!container || container->location != location_) {
            gone.push_back(id);
        }
    }
    for (const std::string& id : gone) {
        if (QWidget* widget = widgets_[id]) {
            stack_->removeWidget(widget);
            delete widget;
        }
        widgets_.erase(id);
    }
}

} // namespace anvil::ui
