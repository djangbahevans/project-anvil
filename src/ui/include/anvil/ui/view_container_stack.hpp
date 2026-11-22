#pragma once

#include "anvil/ui/view_container_registry.hpp"

#include <QPointer>
#include <QStackedWidget>
#include <string>
#include <string_view>
#include <unordered_map>

namespace anvil::ui {

// The widgets of the view containers of one location, one shown at a time. A container's widget is
// made the first time it is shown and kept after that, and goes when the container is no longer
// registered for the location.
class ViewContainerStack {
public:
    // The registry must outlive the stack. The stack widget is a child of `parent`.
    ViewContainerStack(const ViewContainerRegistry& registry, ViewContainerLocation location,
                       QWidget* parent);

    QStackedWidget* widget() const {
        return stack_;
    }

    // Shows the container's widget, making it first if there is none. Null, and nothing shown, for
    // an id that is not registered for the location.
    const ViewContainer* show(std::string_view id);
    // The widget of the container, or null before it was first shown.
    QWidget* container_widget(std::string_view id) const;
    // Deletes the widgets of containers that are no longer registered for the location.
    void prune();

private:
    const ViewContainerRegistry& registry_;
    ViewContainerLocation location_;
    QStackedWidget* stack_ = nullptr;
    std::unordered_map<std::string, QPointer<QWidget>> widgets_;
};

} // namespace anvil::ui
