#pragma once

#include <QIcon>
#include <QObject>
#include <QString>
#include <QWidget>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

enum class ViewContainerLocation {
    sidebar,
    panel,
};

// Something that fills the sidebar or the panel: the explorer, search, the terminal, a tree view of
// an extension. The registry does not know what is inside; `create` makes the widget the first
// time the container is shown.
struct ViewContainer {
    std::string id;
    QString title;
    // A monochrome icon: its shape is drawn in the color the activity bar gives it.
    QIcon icon;
    ViewContainerLocation location = ViewContainerLocation::sidebar;
    // Containers show in ascending order; equal orders keep the order of registration.
    int order = 0;
    std::function<QWidget*()> create;
};

// The view containers that exist, by id. Registering one does not create its widget.
class ViewContainerRegistry : public QObject {
    Q_OBJECT

public:
    // Adds a container, or replaces the one with the same id.
    void add(ViewContainer container);
    // Removes the container; false when there is none with the id.
    bool remove(std::string_view id);

    const ViewContainer* find(std::string_view id) const;
    // The containers of a location in the order they show.
    std::vector<const ViewContainer*> containers(ViewContainerLocation location) const;

signals:
    // A container was added, replaced or removed.
    void changed();

private:
    std::vector<ViewContainer> containers_;
};

} // namespace anvil::ui
