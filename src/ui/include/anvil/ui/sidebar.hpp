#pragma once

#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/view_container_stack.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QObject>
#include <memory>
#include <string>

namespace anvil::ui {

class ActivityBar;

// The activity bar and the sidebar: a button for each view container registered for the sidebar,
// and the container the user chose in the sidebar below a title.
//
// The activity bar shows while there is at least one container. Choosing a container shows the
// sidebar with it; choosing the one that is shown hides the sidebar, as in VS Code. A container's
// widget is made the first time it is shown and kept after that. Showing the sidebar by another
// way, such as the View menu, shows the container that was last active, or the first one.
class Sidebar : public QObject {
    Q_OBJECT

public:
    // The activity bar and the sidebar are filled in the workbench's containers for them. All the
    // arguments must outlive this object.
    Sidebar(Workbench& workbench, ViewContainerRegistry& registry, const WorkbenchColors& colors,
            QObject* parent = nullptr);
    ~Sidebar() override;

    // The id of the container that is shown, or was shown last; empty when none has been.
    const std::string& active_container() const {
        return active_;
    }
    // Shows the sidebar with the container. Nothing happens for an id that is not registered for
    // the sidebar.
    void activate(std::string_view id);

    // The widget of the container, or null before it was first shown.
    QWidget* container_widget(std::string_view id) const;
    // The title above the container, in capitals; empty before one is shown.
    QString title() const;
    // The row at the right end of the title where actions of the container go.
    QWidget* title_actions() const {
        return title_actions_;
    }
    const ActivityBar& activity_bar() const {
        return *activity_bar_;
    }

signals:
    // The active container changed. Empty when there is none left.
    void active_container_changed(const QString& id);

private:
    class Title;

    void refresh();
    void choose(const std::string& id);
    void update_buttons();

    Workbench& workbench_;
    ViewContainerRegistry& registry_;
    ActivityBar* activity_bar_ = nullptr;
    Title* title_ = nullptr;
    QWidget* title_actions_ = nullptr;
    std::unique_ptr<ViewContainerStack> stack_;
    std::string active_;
};

} // namespace anvil::ui
